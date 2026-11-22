#pragma once

#include "DspMath.h"

namespace winaudio::dsp {

// Dedicated Bass Booster (low-shelf + psychoacoustic sub-harmonic exciter for small speakers)
// and Treble / Clarity Booster (high-shelf air + vocal presence bell)
class ToneBooster {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void updateParams(const WaDspParams& params) noexcept;
    void processSample(double& l, double& r) noexcept;

    static double evalResponseDb(const WaDspParams& params, double sampleRate, double freqHz) noexcept;

private:
    double sampleRate_ = 48000.0;
    bool bassEnabled_ = false;
    bool trebleEnabled_ = false;

    StereoBiquad bassShelf_{};
    StereoBiquad trebleShelf_{};
    StereoBiquad clarityBell_{};

    // Psychoacoustic bass exciter path: LPF(115 Hz) -> HPF(35 Hz) -> asymmetric soft saturate -> HPF(90 Hz) -> LPF(360 Hz)
    StereoBiquad exciterInLp_{};
    StereoBiquad exciterInHp_{};
    StereoBiquad exciterOutHp_{};
    StereoBiquad exciterOutLp_{};
    OnePoleSmoother exciterMix_{};
};

} // namespace winaudio::dsp
