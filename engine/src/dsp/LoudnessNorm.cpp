#include "LoudnessNorm.h"

namespace winaudio::dsp {

void LoudnessNorm::prepare(double sampleRate) noexcept {
    sampleRate_ = std::max(8000.0, sampleRate);

    // ITU-R BS.1770 / EBU R128 K-weighting approximation via RBJ biquads
    kPreShelf_.setCoeffs(makeBiquad(WA_FILTER_HIGH_SHELF, sampleRate_, 1681.97, 4.0, 0.707));
    kHighPass_.setCoeffs(makeBiquad(WA_FILTER_HIGH_PASS, sampleRate_, 38.13, 0.0, 0.5));

    // 400ms momentary loudness window; fast downward attack (120ms), smooth upward release (1200ms)
    rmsAlpha_ = 1.0 - std::exp(-1.0 / (0.400 * sampleRate_));
    attackAlpha_ = 1.0 - std::exp(-1.0 / (0.120 * sampleRate_));
    releaseAlpha_ = 1.0 - std::exp(-1.0 / (1.200 * sampleRate_));

    reset();
}

void LoudnessNorm::reset() noexcept {
    kPreShelf_.reset();
    kHighPass_.reset();
    meanSquare_ = 1e-8;
    gainLinear_ = 1.0;
    currentGainDb_ = 0.0f;
}

void LoudnessNorm::updateParams(const WaDspParams& params) noexcept {
    enabled_ = (params.loudnessEnabled != 0);
    targetLufs_ = clampd(params.loudnessTargetLufs, -24.0, -8.0);
    strength_ = clampd(params.loudnessStrength, 0.0, 1.0);
    maxGainDb_ = clampd(params.loudnessMaxGainDb, 0.0, 15.0);
}

void LoudnessNorm::processSample(double& l, double& r) noexcept {
    if (!enabled_ || strength_ <= 1e-4) {
        // Smoothly return gain toward unity when disabled
        gainLinear_ += attackAlpha_ * (1.0 - gainLinear_);
        currentGainDb_ = linearToDb(static_cast<float>(gainLinear_));
        l *= gainLinear_;
        r *= gainLinear_;
        return;
    }

    // K-weighted sidechain loudness detection
    double scL = l;
    double scR = r;
    kPreShelf_.process(scL, scR);
    kHighPass_.process(scL, scR);

    const double instPower = 0.5 * (scL * scL + scR * scR);
    meanSquare_ += rmsAlpha_ * (instPower - meanSquare_);

    // Approximate LUFS (-0.691 offset per BS.1770)
    const double lufs = -0.691 + 10.0 * std::log10(std::max(1e-12, meanSquare_));

    // Noise gate: below -50 LUFS do NOT boost silence/floor noise — gently drift toward unity
    double desiredGainDb = 0.0;
    if (lufs > -50.0) {
        const double diffDb = (targetLufs_ - lufs) * strength_;
        desiredGainDb = clampd(diffDb, -12.0, maxGainDb_);
    } else {
        // Slowly return toward 0 dB during silence so background noise isn't pumped
        desiredGainDb = 0.0;
    }

    const double targetLinear = dbToLinearD(desiredGainDb);
    const double alpha = (targetLinear < gainLinear_) ? attackAlpha_ : releaseAlpha_;
    gainLinear_ += alpha * (targetLinear - gainLinear_);

    l *= gainLinear_;
    r *= gainLinear_;
    currentGainDb_ = linearToDb(static_cast<float>(gainLinear_));
}

} // namespace winaudio::dsp
