#pragma once

#include "RingBuffer.h"
#include <array>
#include <cstdint>
#include <vector>

namespace winaudio::audio {

// Asynchronous Sample Rate Converter (4-point Cubic Hermite / Catmull-Rom)
// + PI Clock-Drift Controller + Smoothly Crossfaded Per-Device Sync Delay Line (0 .. 500 ms).
// Keeps independent hardware clocks (Wired 3.5mm, HDMI, USB, multiple Bluetooth A2DP sinks)
// locked in continuous glitch-free sync over hours of playback.
class DriftResampler {
public:
    void prepare(double inputSampleRate, double outputSampleRate, double targetBufferMs = 25.0) noexcept;
    void reset() noexcept;

    void setTargetBufferMs(double targetBufferMs) noexcept;
    void setDelayMs(double delayMs) noexcept;
    double appliedDelayMs() const noexcept { return currentDelayMs_; }
    float driftPpm() const noexcept { return static_cast<float>(driftPpm_); }
    float smoothedFillMs() const noexcept { return static_cast<float>(smoothedFillMs_); }
    uint32_t underrunCount() const noexcept { return underruns_; }
    uint32_t overrunCount() const noexcept { return overruns_; }

    // Pulls outputFrames from the SPSC ring buffer into interleavedStereoOut (stereo float32).
    // Performs PI-controlled clock drift rate adaptation, sample-rate conversion, and per-device delay.
    void pull(StereoRingBuffer& ring, float* interleavedStereoOut, int outputFrames) noexcept;

private:
    static inline float hermite4(float y0, float y1, float y2, float y3, double frac) noexcept {
        const double f = frac;
        const double c0 = y1;
        const double c1 = 0.5 * (y2 - y0);
        const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
        const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
        return static_cast<float>(((c3 * f + c2) * f + c1) * f + c0);
    }

    double inRate_ = 48000.0;
    double outRate_ = 48000.0;
    double nominalRatio_ = 1.0; // input frames per output frame
    double targetBufferFrames_ = 1200.0;

    // PI drift controller state
    double smoothedFillFrames_ = 1200.0;
    double smoothedFillMs_ = 25.0;
    double piIntegral_ = 0.0;
    double driftPpm_ = 0.0;
    bool primed_ = false;

    // 4-point history for cubic Hermite interpolation
    double phase_ = 0.0;
    std::array<float, 4> histL_{};
    std::array<float, 4> histR_{};

    // Per-device Sync Delay Line (up to 550 ms at outputSampleRate)
    std::vector<float> delayBufL_{};
    std::vector<float> delayBufR_{};
    int delayBufSize_ = 0;
    int delayWriteIdx_ = 0;
    double targetDelaySamples_ = 0.0;
    double currentDelaySamples_ = 0.0;
    double currentDelayMs_ = 0.0;

    uint32_t underruns_ = 0;
    uint32_t overruns_ = 0;
};

} // namespace winaudio::audio
