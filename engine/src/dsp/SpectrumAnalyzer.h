#pragma once

#include "WinAudioABI.h"
#include <array>
#include <atomic>

namespace winaudio::dsp {

// Lock-free lazy 2048-point Radix-2 FFT Spectrum Analyzer -> 64 log-spaced bands (20 Hz .. 20 kHz)
// Audio thread only pushes mono samples into a tiny circular buffer; FFT executes on the UI thread
// ONLY when telemetry requests spectrum data (0% CPU cost when minimized to tray!).
class SpectrumAnalyzer {
public:
    static constexpr int kFftSize = 2048;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    inline void pushMono(float sample) noexcept {
        uint32_t w = writePos_.load(std::memory_order_relaxed);
        ring_[w & (kFftSize - 1)] = sample;
        writePos_.store(w + 1, std::memory_order_release);
    }

    // Called from UI/telemetry thread when spectrum is visible
    void computeSpectrumDb(float outBinsDb[WA_SPECTRUM_BINS]) noexcept;

private:
    double sampleRate_ = 48000.0;
    std::array<float, kFftSize> ring_{};
    std::atomic<uint32_t> writePos_{ 0 };

    std::array<float, kFftSize> window_{};
    std::array<float, WA_SPECTRUM_BINS> smoothedDb_{};
};

} // namespace winaudio::dsp
