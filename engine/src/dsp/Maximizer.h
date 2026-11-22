#pragma once

#include "DspMath.h"
#include <vector>

namespace winaudio::dsp {

// Controlled Sound Booster + 3 ms Lookahead Brickwall Limiter with Soft-Clip Warmth
class Maximizer {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void updateParams(const WaDspParams& params) noexcept;
    void processSample(double& l, double& r) noexcept;

    float gainReductionDb() const noexcept { return gainReductionDb_; }
    int lookaheadSamples() const noexcept { return lookaheadSamples_; }

private:
    double sampleRate_ = 48000.0;
    bool enabled_ = false;

    OnePoleSmoother boostGain_{};
    double ceilingLinear_ = 0.96605; // -0.3 dBFS
    double softClipAmount_ = 0.25;
    double fastReleaseCoeff_ = 0.0;
    double slowReleaseCoeff_ = 0.0;

    int lookaheadSamples_ = 144; // 3 ms @ 48 kHz
    std::vector<double> delayL_{};
    std::vector<double> delayR_{};
    // O(1) monotonic deque storing (sampleIndex, peakMagnitude) for sliding-window max
    std::vector<uint64_t> dequeIdx_{};
    std::vector<double> dequeVal_{};
    int dequeHead_ = 0;
    int dequeTail_ = 0;
    int dequeCap_ = 0;
    uint64_t sampleCounter_ = 0;
    int writeIdx_ = 0;

    double gainEnvelope_ = 1.0;
    double slowEnvelope_ = 1.0;
    float gainReductionDb_ = 0.0f;
};

} // namespace winaudio::dsp
