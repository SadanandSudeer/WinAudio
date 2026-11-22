#pragma once

#include "WinAudioABI.h"
#include "RingBuffer.h"
#include "DriftResampler.h"
#include "dsp/Equalizer.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace winaudio::audio {

// Dedicated MMCSS "Pro Audio" WASAPI Render Worker for a single physical output device.
// Pulls stereo float32 frames from its lock-free SPSC RingBuffer, applies ASRC drift
// compensation, per-device sync delay (0-500ms), optional per-device EQ, volume/balance/mute,
// and writes to the endpoint's native mix format (1ch/2ch/5.1/7.1, float32 or PCM16).
class OutputWorker {
public:
    explicit OutputWorker(const WaOutputSlotConfig& cfg, double engineSampleRate, int bufferMs);
    ~OutputWorker();

    void start();
    void stop();

    void updateConfig(const WaOutputSlotConfig& cfg);
    void setHighHeadroomMode(bool enabled) noexcept {
        highHeadroomMode_.store(enabled, std::memory_order_release);
    }
    void setCalibrationMute(bool muted) noexcept {
        calibrationMute_.store(muted, std::memory_order_release);
    }
    void setCalibrationBypassDelay(bool bypass) noexcept {
        calibrationBypassDelay_.store(bypass, std::memory_order_release);
    }
    void triggerCalibrationChirp() noexcept {
        calibrationChirpTrigger_.store(true, std::memory_order_release);
    }
    bool isBluetooth() const noexcept { return isBluetooth_.load(std::memory_order_relaxed); }
    void setInputSampleRate(int32_t sampleRate) noexcept {
        if (sampleRate >= 8000) {
            inputSampleRate_.store(sampleRate, std::memory_order_release);
        }
    }
    const std::wstring& deviceId() const noexcept { return deviceId_; }

    // Called from CaptureWorker audio thread (lock-free, zero allocations)
    inline void pushAudio(const float* interleavedStereo, uint32_t numFrames, int32_t captureSampleRate) noexcept {
        if (captureSampleRate >= 8000 && inputSampleRate_.load(std::memory_order_relaxed) != captureSampleRate) {
            inputSampleRate_.store(captureSampleRate, std::memory_order_release);
        }
        if (!running_.load(std::memory_order_relaxed)) return;
        ring_.write(interleavedStereo, numFrames);
    }

    WaOutputTelemetry getTelemetry() const noexcept;

private:
    void workerLoop();

    std::wstring deviceId_;
    std::atomic<int32_t> inputSampleRate_{ 48000 };
    int bufferMs_ = 20;

    StereoRingBuffer ring_;
    DriftResampler resampler_;
    dsp::Equalizer customEq_;

    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> running_{ false };
    std::thread thread_;

    // Lock-free atomic scalar controls
    std::atomic<float> volume_{ 1.0f };
    std::atomic<bool> mute_{ false };
    std::atomic<float> balance_{ 0.0f };
    std::atomic<float> syncDelayMs_{ 0.0f };
    std::atomic<bool> useCustomEq_{ false };
    std::atomic<bool> highHeadroomMode_{ false };
    std::atomic<bool> isBluetooth_{ false };
    std::atomic<bool> calibrationMute_{ false };
    std::atomic<bool> calibrationBypassDelay_{ false };
    std::atomic<bool> calibrationChirpTrigger_{ false };

    // Double-buffered per-device EQ params
    std::mutex eqMutex_;
    WaEqParams pendingEq_{};
    std::atomic<uint32_t> eqSeq_{ 0 };

    // Telemetry atomics
    std::atomic<int32_t> state_{ WA_OUTPUT_IDLE };
    std::atomic<int32_t> endpointSampleRate_{ 48000 };
    std::atomic<float> bufferFillMs_{ 0.0f };
    std::atomic<float> driftPpm_{ 0.0f };
    std::atomic<float> appliedDelayMs_{ 0.0f };
    std::atomic<uint32_t> underruns_{ 0 };
    std::atomic<uint32_t> overruns_{ 0 };
};

} // namespace winaudio::audio
