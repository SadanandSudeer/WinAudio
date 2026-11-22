#pragma once

#include "WinAudioABI.h"
#include "Equalizer.h"
#include "ToneBooster.h"
#include "LoudnessNorm.h"
#include "SpatialEffects.h"
#include "Maximizer.h"
#include "SpectrumAnalyzer.h"
#include <atomic>
#include <vector>

namespace winaudio::dsp {

struct DspMeterSnapshot {
    float inputPeakDbL = kMinDb;
    float inputPeakDbR = kMinDb;
    float inputRmsDbL = kMinDb;
    float inputRmsDbR = kMinDb;
    float outputPeakDbL = kMinDb;
    float outputPeakDbR = kMinDb;
    float outputRmsDbL = kMinDb;
    float outputRmsDbR = kMinDb;
    float limiterGrDb = 0.0f;
    float loudnessGainDb = 0.0f;
};

// Complete real-time Master DSP Chain with lock-free double-buffered parameter updates
// and click-free 5ms bypass crossfade. Zero heap allocations during processBlock().
class MasterDspChain {
public:
    MasterDspChain() noexcept;
    static WaDspParams defaultParams() noexcept;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    // Thread-safe parameter updates (called from UI thread)
    void setParams(const WaDspParams& params) noexcept;
    void setBypass(bool bypass) noexcept;
    bool isBypassed() const noexcept { return bypassOverride_.load(std::memory_order_relaxed); }

    // Real-time audio thread processing (interleaved stereo float32, in-place)
    void processBlock(float* interleavedStereo, int numFrames) noexcept;

    // Telemetry access (UI thread)
    DspMeterSnapshot getMeters() const noexcept;
    void computeSpectrum(float outBinsDb[WA_SPECTRUM_BINS]) noexcept {
        spectrum_.computeSpectrumDb(outBinsDb);
    }

    static void computeAnalyticalCurve(const WaDspParams& params, float sampleRate, const float* freqsHz, float* outDb, int count) noexcept;

private:
    void applyPendingParamsIfNeeded() noexcept;

    double sampleRate_ = 48000.0;

    // Lock-free triple-slot param handoff (UI writes pending -> increments seq -> audio thread reads)
    WaDspParams paramSlots_[2]{};
    std::atomic<int> activeSlot_{ 0 };
    std::atomic<uint32_t> paramSeq_{ 0 };
    uint32_t appliedSeq_ = 0;
    std::atomic<bool> bypassOverride_{ false };

    // DSP stages
    StereoBiquad dcBlocker_{};
    OnePoleSmoother preAmpGain_{};
    ToneBooster toneBooster_{};
    Equalizer equalizer_{};
    LoudnessNorm loudnessNorm_{};
    SpatialEffects spatial_{};
    Maximizer maximizer_{};
    SpectrumAnalyzer spectrum_{};

    // Click-free bypass crossfade (0.0 = dry bypass, 1.0 = wet processed)
    // Dry path delay-matched to Maximizer lookahead so bypass toggle never glitches phase!
    std::vector<float> dryDelayL_{};
    std::vector<float> dryDelayR_{};
    int dryWriteIdx_ = 0;
    float wetMix_ = 1.0f;
    float wetStepPerSample_ = 0.004f; // ~5 ms ramp @ 48 kHz

    // Atomic meters (published at end of each block)
    std::atomic<float> inPeakL_{ kMinDb }, inPeakR_{ kMinDb };
    std::atomic<float> inRmsL_{ kMinDb }, inRmsR_{ kMinDb };
    std::atomic<float> outPeakL_{ kMinDb }, outPeakR_{ kMinDb };
    std::atomic<float> outRmsL_{ kMinDb }, outRmsR_{ kMinDb };
    std::atomic<float> limGrDb_{ 0.0f }, loudGainDb_{ 0.0f };
};

} // namespace winaudio::dsp
