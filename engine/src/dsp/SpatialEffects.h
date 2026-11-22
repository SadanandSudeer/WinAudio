#pragma once

#include "DspMath.h"
#include <array>
#include <vector>

namespace winaudio::dsp {

// Spatial Suite:
// 1. Mid/Side Stereo Widener with 2nd-order Linkwitz-Riley Bass-Mono crossover
// 2. Virtual Surround (Binaural Headphone ITD/head-shadow + early reflections, or Speaker spatializer)
// 3. Lightweight 4-channel Hadamard FDN Ambience (Room Simulation)
class SpatialEffects {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void updateParams(const WaDspParams& params) noexcept;
    void processSample(double& l, double& r) noexcept;

private:
    double sampleRate_ = 48000.0;

    // 1. Stereo Widener
    bool widenerEnabled_ = false;
    OnePoleSmoother widthSmoother_{};
    StereoBiquad bassLp_{};
    StereoBiquad bassHp_{};
    double lastBassMonoFreq_ = 150.0;

    // 2. Virtual Surround
    WaSurroundMode surroundMode_ = WA_SURROUND_OFF;
    OnePoleSmoother surroundDepth_{};
    static constexpr int kSurroundDelaySize = 2048; // > 40ms @ 48kHz
    std::array<double, kSurroundDelaySize> surrBufL_{};
    std::array<double, kSurroundDelaySize> surrBufR_{};
    int surrWriteIdx_ = 0;
    int itdSamples_ = 18;
    int erTap1_ = 380;
    int erTap2_ = 610;
    int erTap3_ = 890;
    double headShadowL_ = 0.0;
    double headShadowR_ = 0.0;
    double headShadowAlpha_ = 0.2;

    // 3. 4-channel Hadamard FDN Ambience
    bool ambienceEnabled_ = false;
    OnePoleSmoother ambienceMix_{};
    double ambienceFeedback_ = 0.55;
    double ambienceDampAlpha_ = 0.4;
    static constexpr int kFdnMaxDelay = 4096;
    std::array<std::array<double, kFdnMaxDelay>, 4> fdnBuf_{};
    std::array<int, 4> fdnWriteIdx_{};
    std::array<int, 4> fdnDelayLen_{ 1123, 1487, 1901, 2377 };
    std::array<double, 4> fdnDampState_{};
};

} // namespace winaudio::dsp
