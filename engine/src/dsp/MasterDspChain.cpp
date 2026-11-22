#include "MasterDspChain.h"
#include <cstring>

#ifdef _MSC_VER
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace winaudio::dsp {

MasterDspChain::MasterDspChain() noexcept {
    paramSlots_[0] = defaultParams();
    paramSlots_[1] = paramSlots_[0];
    paramSeq_.store(1, std::memory_order_relaxed);
    appliedSeq_ = 0;
}

WaDspParams MasterDspChain::defaultParams() noexcept {
    WaDspParams p{};
    p.globalBypass = 0;
    p.preAmpDb = 0.0f;

    p.bassEnabled = 0;
    p.bassBoostDb = 0.0f;
    p.bassFreqHz = 90.0f;
    p.bassExciterMix = 0.0f;

    p.trebleEnabled = 0;
    p.trebleBoostDb = 0.0f;
    p.trebleFreqHz = 6000.0f;
    p.clarityBoostDb = 0.0f;

    p.eq.enabled = 1;
    p.eq.mode = WA_EQ_10BAND;
    for (int i = 0; i < WA_MAX_GRAPHIC_BANDS; ++i) {
        p.eq.graphicGainsDb[i] = 0.0f;
    }

    constexpr float defaultParamFreqs[WA_MAX_PARAM_BANDS] = {
        32.0f, 64.0f, 125.0f, 250.0f, 500.0f, 1000.0f,
        2000.0f, 4000.0f, 6500.0f, 8000.0f, 12000.0f, 16000.0f
    };
    for (int i = 0; i < WA_MAX_PARAM_BANDS; ++i) {
        p.eq.paramBands[i].enabled = (i < 8) ? 1 : 0;
        p.eq.paramBands[i].filterType = (i == 0) ? WA_FILTER_LOW_SHELF :
                                        (i == 11 ? WA_FILTER_HIGH_SHELF : WA_FILTER_PEAKING);
        p.eq.paramBands[i].frequencyHz = defaultParamFreqs[i];
        p.eq.paramBands[i].gainDb = 0.0f;
        p.eq.paramBands[i].q = 1.2f;
    }

    p.loudnessEnabled = 0;
    p.loudnessTargetLufs = -15.0f;
    p.loudnessStrength = 0.65f;
    p.loudnessMaxGainDb = 9.0f;

    p.widenerEnabled = 0;
    p.stereoWidth = 1.0f;
    p.bassMonoFreqHz = 150.0f;

    p.surroundMode = WA_SURROUND_OFF;
    p.surroundDepth = 0.45f;

    p.ambienceEnabled = 0;
    p.ambienceRoomSize = 0.45f;
    p.ambienceDamping = 0.55f;
    p.ambienceMix = 0.18f;

    p.maximizerEnabled = 1;
    p.maximizerBoostDb = 0.0f;
    p.maximizerCeilingDb = -0.3f;
    p.maximizerSoftClip = 0.2f;
    p.maximizerReleaseMs = 80.0f;
    return p;
}

void MasterDspChain::prepare(double sampleRate) noexcept {
    sampleRate_ = std::max(8000.0, sampleRate);

    dcBlocker_.setCoeffs(makeBiquad(WA_FILTER_HIGH_PASS, sampleRate_, 10.0, 0.0, 0.707));
    preAmpGain_.prepare(sampleRate_, 15.0, 1.0f);
    toneBooster_.prepare(sampleRate_);
    equalizer_.prepare(sampleRate_);
    loudnessNorm_.prepare(sampleRate_);
    spatial_.prepare(sampleRate_);
    maximizer_.prepare(sampleRate_);
    spectrum_.prepare(sampleRate_);

    // Delay-match dry bypass line to Maximizer's 3 ms lookahead for phase-clean click-free toggling
    const int dryDelayLen = std::max(1, maximizer_.lookaheadSamples());
    dryDelayL_.assign(dryDelayLen, 0.0f);
    dryDelayR_.assign(dryDelayLen, 0.0f);
    dryWriteIdx_ = 0;

    wetStepPerSample_ = static_cast<float>(1.0 / std::max(1.0, sampleRate_ * 0.005)); // 5 ms ramp

    // Preserve any user-configured DSP params already loaded, and force recalculation for new sampleRate
    appliedSeq_ = paramSeq_.load(std::memory_order_acquire) - 1;
    applyPendingParamsIfNeeded();
    reset();
}

void MasterDspChain::reset() noexcept {
    dcBlocker_.reset();
    toneBooster_.reset();
    equalizer_.reset();
    loudnessNorm_.reset();
    spatial_.reset();
    maximizer_.reset();
    spectrum_.reset();
    std::fill(dryDelayL_.begin(), dryDelayL_.end(), 0.0f);
    std::fill(dryDelayR_.begin(), dryDelayR_.end(), 0.0f);
    dryWriteIdx_ = 0;
}

void MasterDspChain::setParams(const WaDspParams& params) noexcept {
    int nextSlot = 1 - activeSlot_.load(std::memory_order_relaxed);
    paramSlots_[nextSlot] = params;
    bypassOverride_.store(params.globalBypass != 0, std::memory_order_relaxed);
    activeSlot_.store(nextSlot, std::memory_order_release);
    paramSeq_.fetch_add(1, std::memory_order_release);
}

void MasterDspChain::setBypass(bool bypass) noexcept {
    bypassOverride_.store(bypass, std::memory_order_relaxed);
}

void MasterDspChain::applyPendingParamsIfNeeded() noexcept {
    const uint32_t seq = paramSeq_.load(std::memory_order_acquire);
    if (seq == appliedSeq_) return;

    const int slot = activeSlot_.load(std::memory_order_acquire);
    const WaDspParams& p = paramSlots_[slot];
    appliedSeq_ = seq;

    preAmpGain_.setTarget(dbToLinear(clampf(p.preAmpDb, -18.0f, 24.0f)));
    toneBooster_.updateParams(p);
    equalizer_.updateParams(p.eq);
    loudnessNorm_.updateParams(p);
    spatial_.updateParams(p);
    maximizer_.updateParams(p);
}

void MasterDspChain::processBlock(float* interleavedStereo, int numFrames) noexcept {
    if (!interleavedStereo || numFrames <= 0) return;

#ifdef _MSC_VER
    // Flush subnormal/denormal floats to zero to avoid huge CPU spikes during silence
    const unsigned int oldFtz = _MM_GET_FLUSH_ZERO_MODE();
    const unsigned int oldDaz = _MM_GET_DENORMALS_ZERO_MODE();
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif

    applyPendingParamsIfNeeded();

    const bool bypassed = bypassOverride_.load(std::memory_order_relaxed);
    const float targetWet = bypassed ? 0.0f : 1.0f;
    const int dryLen = static_cast<int>(dryDelayL_.size());

    float inPkL = 0.0f, inPkR = 0.0f;
    double inSqL = 0.0, inSqR = 0.0;
    float outPkL = 0.0f, outPkR = 0.0f;
    double outSqL = 0.0, outSqR = 0.0;

    for (int i = 0; i < numFrames; ++i) {
        const float rawL = interleavedStereo[i * 2];
        const float rawR = interleavedStereo[i * 2 + 1];

        inPkL = std::max(inPkL, std::abs(rawL));
        inPkR = std::max(inPkR, std::abs(rawR));
        inSqL += static_cast<double>(rawL) * rawL;
        inSqR += static_cast<double>(rawR) * rawR;

        // Delay-matched dry signal for click-free bypass crossfade
        const float dryL = dryDelayL_[dryWriteIdx_];
        const float dryR = dryDelayR_[dryWriteIdx_];
        dryDelayL_[dryWriteIdx_] = rawL;
        dryDelayR_[dryWriteIdx_] = rawR;
        dryWriteIdx_++;
        if (dryWriteIdx_ >= dryLen) dryWriteIdx_ = 0;

        // Smooth 5ms ramp on bypass transition
        if (wetMix_ < targetWet) {
            wetMix_ = std::min(targetWet, wetMix_ + wetStepPerSample_);
        } else if (wetMix_ > targetWet) {
            wetMix_ = std::max(targetWet, wetMix_ - wetStepPerSample_);
        }

        float finalL = dryL;
        float finalR = dryR;

        if (wetMix_ > 0.0f) {
            const double pre = static_cast<double>(preAmpGain_.next());
            double l = static_cast<double>(rawL) * pre;
            double r = static_cast<double>(rawR) * pre;

            dcBlocker_.process(l, r);
            toneBooster_.processSample(l, r);
            equalizer_.processSample(l, r);
            loudnessNorm_.processSample(l, r);
            spatial_.processSample(l, r);
            maximizer_.processSample(l, r);

            // Sanitize NaN/Inf defensively
            if (!std::isfinite(l)) l = 0.0;
            if (!std::isfinite(r)) r = 0.0;

            const float wetL = static_cast<float>(l);
            const float wetR = static_cast<float>(r);

            finalL = dryL + wetMix_ * (wetL - dryL);
            finalR = dryR + wetMix_ * (wetR - dryR);
        }

        interleavedStereo[i * 2] = finalL;
        interleavedStereo[i * 2 + 1] = finalR;

        spectrum_.pushMono(0.5f * (finalL + finalR));

        outPkL = std::max(outPkL, std::abs(finalL));
        outPkR = std::max(outPkR, std::abs(finalR));
        outSqL += static_cast<double>(finalL) * finalL;
        outSqR += static_cast<double>(finalR) * finalR;
    }

    const double invN = 1.0 / std::max(1, numFrames);
    inPeakL_.store(linearToDb(inPkL), std::memory_order_relaxed);
    inPeakR_.store(linearToDb(inPkR), std::memory_order_relaxed);
    inRmsL_.store(linearToDb(static_cast<float>(std::sqrt(inSqL * invN))), std::memory_order_relaxed);
    inRmsR_.store(linearToDb(static_cast<float>(std::sqrt(inSqR * invN))), std::memory_order_relaxed);

    outPeakL_.store(linearToDb(outPkL), std::memory_order_relaxed);
    outPeakR_.store(linearToDb(outPkR), std::memory_order_relaxed);
    outRmsL_.store(linearToDb(static_cast<float>(std::sqrt(outSqL * invN))), std::memory_order_relaxed);
    outRmsR_.store(linearToDb(static_cast<float>(std::sqrt(outSqR * invN))), std::memory_order_relaxed);

    limGrDb_.store(bypassed ? 0.0f : maximizer_.gainReductionDb(), std::memory_order_relaxed);
    loudGainDb_.store(bypassed ? 0.0f : loudnessNorm_.currentGainDb(), std::memory_order_relaxed);

#ifdef _MSC_VER
    _MM_SET_FLUSH_ZERO_MODE(oldFtz);
    _MM_SET_DENORMALS_ZERO_MODE(oldDaz);
#endif
}

DspMeterSnapshot MasterDspChain::getMeters() const noexcept {
    DspMeterSnapshot m{};
    m.inputPeakDbL = inPeakL_.load(std::memory_order_relaxed);
    m.inputPeakDbR = inPeakR_.load(std::memory_order_relaxed);
    m.inputRmsDbL = inRmsL_.load(std::memory_order_relaxed);
    m.inputRmsDbR = inRmsR_.load(std::memory_order_relaxed);
    m.outputPeakDbL = outPeakL_.load(std::memory_order_relaxed);
    m.outputPeakDbR = outPeakR_.load(std::memory_order_relaxed);
    m.outputRmsDbL = outRmsL_.load(std::memory_order_relaxed);
    m.outputRmsDbR = outRmsR_.load(std::memory_order_relaxed);
    m.limiterGrDb = limGrDb_.load(std::memory_order_relaxed);
    m.loudnessGainDb = loudGainDb_.load(std::memory_order_relaxed);
    return m;
}

void MasterDspChain::computeAnalyticalCurve(const WaDspParams& params, float sampleRate, const float* freqsHz, float* outDb, int count) noexcept {
    if (!freqsHz || !outDb || count <= 0) return;
    const double sr = std::max(8000.0, static_cast<double>(sampleRate));
    for (int i = 0; i < count; ++i) {
        double f = clampd(static_cast<double>(freqsHz[i]), 10.0, sr * 0.49);
        double db = static_cast<double>(params.preAmpDb);
        db += ToneBooster::evalResponseDb(params, sr, f);
        db += Equalizer::evalResponseDb(params.eq, sr, f);
        outDb[i] = static_cast<float>(db);
    }
}

} // namespace winaudio::dsp
