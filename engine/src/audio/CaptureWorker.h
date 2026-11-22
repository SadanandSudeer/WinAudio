#pragma once

#include "WinAudioABI.h"
#include "dsp/MasterDspChain.h"
#include "OutputWorker.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace winaudio::audio {

// Captures system audio from either:
// - Paired Virtual Capture endpoint (e.g. VB-CABLE Output), OR
// - WASAPI Loopback on a render endpoint (with silent keepalive stream so loopback never stalls).
// Runs MasterDspChain on MMCSS "Pro Audio" thread and fans out stereo float32 frames
// to all active OutputWorkers simultaneously.
class CaptureWorker {
public:
    CaptureWorker(dsp::MasterDspChain& dspChain);
    ~CaptureWorker();

    // captureEndpointId: endpoint to capture; isLoopback=true if render endpoint loopback
    void start(const std::wstring& captureEndpointId, bool isLoopback, int bufferMs);
    void stop();

    // Atomically swaps the active fan-out OutputWorker list (shared_ptr snapshot)
    void setOutputs(std::shared_ptr<std::vector<std::shared_ptr<OutputWorker>>> outputs);

    void triggerSyncPulse() noexcept;
    void setTestTone(bool enabled, float freqHz, float gainDb) noexcept;

    int32_t sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    float cpuLoadPercent() const noexcept { return cpuLoadPercent_.load(std::memory_order_relaxed); }
    bool isRunning() const noexcept { return running_.load(std::memory_order_relaxed); }

private:
    void workerLoop();
    void injectTestAndPulse(float* interleavedStereo, int numFrames, double sampleRate) noexcept;

    dsp::MasterDspChain& dsp_;

    std::wstring endpointId_;
    bool isLoopback_ = true;
    int bufferMs_ = 15;

    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> running_{ false };
    std::atomic<int32_t> sampleRate_{ 48000 };
    std::atomic<float> cpuLoadPercent_{ 0.0f };
    std::thread thread_;

    std::mutex outputsMutex_;
    std::shared_ptr<std::vector<std::shared_ptr<OutputWorker>>> outputs_;
    std::atomic<uint64_t> outputsVersion_{ 0 };

    // Sync Pulse (35ms windowed 1000 Hz ping for aligning Wired + Bluetooth delay)
    std::atomic<int> syncPulseSamplesRemaining_{ 0 };
    double syncPulsePhase_ = 0.0;

    // Built-in Test Tone (lets users verify EQ/Maximizer/Spatial even without playing external audio)
    std::atomic<bool> testToneEnabled_{ false };
    std::atomic<float> testToneFreq_{ 440.0f };
    std::atomic<float> testToneGain_{ 0.25f };
    double testTonePhase_ = 0.0;
};

} // namespace winaudio::audio
