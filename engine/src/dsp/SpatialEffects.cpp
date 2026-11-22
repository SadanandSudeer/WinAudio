#include "SpatialEffects.h"

namespace winaudio::dsp {

void SpatialEffects::prepare(double sampleRate) noexcept {
    sampleRate_ = std::max(8000.0, sampleRate);
    widthSmoother_.prepare(sampleRate_, 20.0, 1.0f);
    surroundDepth_.prepare(sampleRate_, 25.0, 0.0f);
    ambienceMix_.prepare(sampleRate_, 25.0, 0.0f);

    lastBassMonoFreq_ = 150.0;
    // Linkwitz-Riley 2nd order crossover (Q = 0.5)
    bassLp_.setCoeffs(makeBiquad(WA_FILTER_LOW_PASS, sampleRate_, lastBassMonoFreq_, 0.0, 0.5));
    bassHp_.setCoeffs(makeBiquad(WA_FILTER_HIGH_PASS, sampleRate_, lastBassMonoFreq_, 0.0, 0.5));

    // ITD ~0.42 ms, early lateral reflections ~8 ms, 13 ms, 19 ms
    itdSamples_ = std::clamp(static_cast<int>(std::round(sampleRate_ * 0.00042)), 4, 128);
    erTap1_ = std::clamp(static_cast<int>(std::round(sampleRate_ * 0.0081)), 16, kSurroundDelaySize - 2);
    erTap2_ = std::clamp(static_cast<int>(std::round(sampleRate_ * 0.0133)), 32, kSurroundDelaySize - 2);
    erTap3_ = std::clamp(static_cast<int>(std::round(sampleRate_ * 0.0191)), 48, kSurroundDelaySize - 2);

    // Head-shadow 1-pole LPF ~1.8 kHz
    headShadowAlpha_ = 1.0 - std::exp(-kTwoPi * 1800.0 / sampleRate_);

    reset();
}

void SpatialEffects::reset() noexcept {
    bassLp_.reset();
    bassHp_.reset();
    surrBufL_.fill(0.0);
    surrBufR_.fill(0.0);
    surrWriteIdx_ = 0;
    headShadowL_ = headShadowR_ = 0.0;

    for (auto& ch : fdnBuf_) ch.fill(0.0);
    fdnWriteIdx_.fill(0);
    fdnDampState_.fill(0.0);
}

void SpatialEffects::updateParams(const WaDspParams& params) noexcept {
    widenerEnabled_ = (params.widenerEnabled != 0);
    widthSmoother_.setTarget(widenerEnabled_ ? clampf(params.stereoWidth, 0.0f, 2.0f) : 1.0f);

    double bmFreq = clampd(params.bassMonoFreqHz, 80.0, 300.0);
    if (std::abs(bmFreq - lastBassMonoFreq_) > 0.5) {
        lastBassMonoFreq_ = bmFreq;
        bassLp_.setCoeffs(makeBiquad(WA_FILTER_LOW_PASS, sampleRate_, bmFreq, 0.0, 0.5));
        bassHp_.setCoeffs(makeBiquad(WA_FILTER_HIGH_PASS, sampleRate_, bmFreq, 0.0, 0.5));
    }

    surroundMode_ = static_cast<WaSurroundMode>(std::clamp(params.surroundMode, 0, 2));
    surroundDepth_.setTarget(surroundMode_ != WA_SURROUND_OFF ? clampf(params.surroundDepth, 0.0f, 1.0f) : 0.0f);

    ambienceEnabled_ = (params.ambienceEnabled != 0);
    ambienceMix_.setTarget(ambienceEnabled_ ? clampf(params.ambienceMix, 0.0f, 0.65f) : 0.0f);

    const double room = clampd(params.ambienceRoomSize, 0.1, 1.0);
    const double damp = clampd(params.ambienceDamping, 0.0, 1.0);

    // Keep FDN strictly passive & stable: max feedback 0.80 (with 0.5 Hadamard normalization)
    ambienceFeedback_ = 0.35 + room * 0.43;
    double dampCutoffHz = 12000.0 - damp * 9500.0; // 12 kHz .. 2.5 kHz
    ambienceDampAlpha_ = clampd(1.0 - std::exp(-kTwoPi * dampCutoffHz / sampleRate_), 0.05, 0.95);

    const double scale = (0.45 + 0.55 * room) * (sampleRate_ / 48000.0);
    constexpr int basePrimes[4] = { 1097, 1481, 1933, 2417 };
    for (int i = 0; i < 4; ++i) {
        fdnDelayLen_[i] = std::clamp(static_cast<int>(std::round(basePrimes[i] * scale)), 64, kFdnMaxDelay - 2);
    }
}

void SpatialEffects::processSample(double& l, double& r) noexcept {
    // 1. Mid/Side Stereo Widener with Bass-Mono Preservation
    const double width = static_cast<double>(widthSmoother_.next());
    if (widenerEnabled_ || std::abs(width - 1.0) > 1e-4) {
        double lowL = l, lowR = r;
        double highL = l, highR = r;
        bassLp_.process(lowL, lowR);
        bassHp_.process(highL, highR);

        // LR2 crossover polarity alignment (invert high band to sum flat with low band)
        highL = -highL;
        highR = -highR;

        // Keep bass cleanly centered/mono so widening never causes hollow phase cancellation
        const double lowMono = 0.5 * (lowL + lowR);

        // Mid/Side widening on high band with energy compensation
        const double mid = 0.5 * (highL + highR);
        const double side = 0.5 * (highL - highR) * width;
        const double norm = 1.0 / std::sqrt(0.5 * (1.0 + width * width));

        l = lowMono + (mid + side) * norm;
        r = lowMono + (mid - side) * norm;
    }

    // 2. Virtual Surround (Binaural Headphones or Speaker Virtualizer)
    const double surrDepth = static_cast<double>(surroundDepth_.next());
    surrBufL_[surrWriteIdx_] = l;
    surrBufR_[surrWriteIdx_] = r;

    if (surrDepth > 1e-4) {
        auto readDelay = [&](const std::array<double, kSurroundDelaySize>& buf, int d) noexcept {
            int idx = (surrWriteIdx_ - d + kSurroundDelaySize) & (kSurroundDelaySize - 1);
            return buf[idx];
        };

        if (surroundMode_ == WA_SURROUND_HEADPHONES) {
            // Binaural crossfeed (ITD + head-shadow LPF) + decorrelated lateral early reflections
            const double crossRtoL = readDelay(surrBufR_, itdSamples_);
            const double crossLtoR = readDelay(surrBufL_, itdSamples_);

            headShadowL_ += headShadowAlpha_ * (crossRtoL - headShadowL_);
            headShadowR_ += headShadowAlpha_ * (crossLtoR - headShadowR_);

            const double erL = 0.22 * readDelay(surrBufL_, erTap1_)
                             - 0.16 * readDelay(surrBufR_, erTap2_)
                             + 0.11 * readDelay(surrBufL_, erTap3_);
            const double erR = 0.22 * readDelay(surrBufR_, erTap1_)
                             - 0.16 * readDelay(surrBufL_, erTap2_)
                             + 0.11 * readDelay(surrBufR_, erTap3_);

            const double wetL = l * 0.88 + headShadowL_ * 0.22 + erL * 0.42;
            const double wetR = r * 0.88 + headShadowR_ * 0.22 + erR * 0.42;

            l = l + surrDepth * (wetL - l);
            r = r + surrDepth * (wetR - r);
        } else if (surroundMode_ == WA_SURROUND_SPEAKERS) {
            // Speaker soundstage virtualizer: subtle side-delayed spatial expansion + depth taps
            const double sideDiff = 0.5 * (readDelay(surrBufL_, itdSamples_) - readDelay(surrBufR_, itdSamples_));
            const double rearL = 0.18 * readDelay(surrBufL_, erTap1_) - 0.14 * readDelay(surrBufR_, erTap2_);
            const double rearR = 0.18 * readDelay(surrBufR_, erTap1_) - 0.14 * readDelay(surrBufL_, erTap2_);

            const double wetL = l + 0.26 * sideDiff + 0.35 * rearL;
            const double wetR = r - 0.26 * sideDiff + 0.35 * rearR;

            l = l + surrDepth * (wetL - l);
            r = r + surrDepth * (wetR - r);
        }
    }
    surrWriteIdx_ = (surrWriteIdx_ + 1) & (kSurroundDelaySize - 1);

    // 3. 4-Channel Hadamard FDN Room Ambience
    const double ambMix = static_cast<double>(ambienceMix_.next());
    if (ambienceEnabled_ || ambMix > 1e-4) {
        double y[4];
        for (int i = 0; i < 4; ++i) {
            int readIdx = (fdnWriteIdx_[i] - fdnDelayLen_[i] + kFdnMaxDelay) & (kFdnMaxDelay - 1);
            double raw = fdnBuf_[i][readIdx];
            // Per-line HF damping low-pass filter
            fdnDampState_[i] += ambienceDampAlpha_ * (raw - fdnDampState_[i]);
            y[i] = fdnDampState_[i];
        }

        // Normalized 4x4 Hadamard orthogonal mixing matrix (energy preserving!)
        const double h0 = 0.5 * (y[0] + y[1] + y[2] + y[3]);
        const double h1 = 0.5 * (y[0] - y[1] + y[2] - y[3]);
        const double h2 = 0.5 * (y[0] + y[1] - y[2] - y[3]);
        const double h3 = 0.5 * (y[0] - y[1] - y[2] + y[3]);

        const double inL = l * 0.5;
        const double inR = r * 0.5;
        const double fb = ambienceFeedback_;

        fdnBuf_[0][fdnWriteIdx_[0]] = inL + fb * h0;
        fdnBuf_[1][fdnWriteIdx_[1]] = inR + fb * h1;
        fdnBuf_[2][fdnWriteIdx_[2]] = (inL - inR) * 0.707 + fb * h2;
        fdnBuf_[3][fdnWriteIdx_[3]] = (inL + inR) * 0.707 + fb * h3;

        for (int i = 0; i < 4; ++i) {
            fdnWriteIdx_[i] = (fdnWriteIdx_[i] + 1) & (kFdnMaxDelay - 1);
        }

        const double wetL = 0.5 * (y[0] + y[2]);
        const double wetR = 0.5 * (y[1] + y[3]);

        l = l * (1.0 - ambMix * 0.35) + wetL * ambMix;
        r = r * (1.0 - ambMix * 0.35) + wetR * ambMix;
    }
}

} // namespace winaudio::dsp
