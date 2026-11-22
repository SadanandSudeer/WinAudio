#pragma once

#include "DspMath.h"
#include <array>

namespace winaudio::dsp {

inline constexpr std::array<double, 10> kEq10Freqs = {
    31.25, 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0
};

inline constexpr std::array<double, 31> kEq31Freqs = {
    20.0, 25.0, 31.5, 40.0, 50.0, 63.0, 80.0, 100.0, 125.0, 160.0,
    200.0, 250.0, 315.0, 400.0, 500.0, 630.0, 800.0, 1000.0, 1250.0, 1600.0,
    2000.0, 2500.0, 3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0, 20000.0
};

class Equalizer {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void updateParams(const WaEqParams& params) noexcept;

    inline void processSample(double& l, double& r) noexcept {
        if (!enabled_) return;
        for (int i = 0; i < activeCount_; ++i) {
            bands_[activeIndices_[i]].process(l, r);
        }
    }

    static double evalResponseDb(const WaEqParams& params, double sampleRate, double freqHz) noexcept;

private:
    double sampleRate_ = 48000.0;
    bool enabled_ = false;
    std::array<StereoBiquad, WA_MAX_GRAPHIC_BANDS> bands_{};
    std::array<int, WA_MAX_GRAPHIC_BANDS> activeIndices_{};
    int activeCount_ = 0;
};

} // namespace winaudio::dsp
