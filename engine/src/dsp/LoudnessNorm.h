#pragma once

#include "DspMath.h"

namespace winaudio::dsp {

// EBU R128 K-Weighted Gated Loudness Normalizer (Dynamic Leveler / AGC)
class LoudnessNorm {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void updateParams(const WaDspParams& params) noexcept;
    void processSample(double& l, double& r) noexcept;

    float currentGainDb() const noexcept { return currentGainDb_; }

private:
    double sampleRate_ = 48000.0;
    bool enabled_ = false;
    double targetLufs_ = -15.0;
    double strength_ = 0.65;
    double maxGainDb_ = 9.0;

    // K-weighting sidechain filters (ITU-R BS.1770)
    StereoBiquad kPreShelf_{};
    StereoBiquad kHighPass_{};

    double rmsAlpha_ = 0.0;
    double attackAlpha_ = 0.0;
    double releaseAlpha_ = 0.0;
    double meanSquare_ = 1e-8;
    double gainLinear_ = 1.0;
    float currentGainDb_ = 0.0f;
};

} // namespace winaudio::dsp
