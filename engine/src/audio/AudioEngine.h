#pragma once

#include "WinAudioABI.h"
#include "DeviceManager.h"
#include "CaptureWorker.h"
#include "OutputWorker.h"
#include "dsp/MasterDspChain.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace winaudio::audio {

// Top-level orchestrator for WinAudioEngine.dll.
// Manages Hybrid Virtual Sink (Full System DSP) + Zero-Driver Loopback fallback,
// safe default-device restore on stop/destroy, multi-device fan-out, and telemetry.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool start(const std::wstring& preferredVirtualSinkId, bool preferVirtualMode, int bufferMs);
    void stop();

    WaRoutingMode routingMode() const noexcept { return routingMode_.load(std::memory_order_relaxed); }

    void setMasterDsp(const WaDspParams& params) noexcept;
    void setBypass(bool bypass) noexcept;

    std::vector<WaDeviceInfo> enumerateDevices();
    bool setDefaultDevice(const std::wstring& deviceId, bool includeCommunications);
    bool configureOutputs(const WaOutputSlotConfig* slots, int count);

    void triggerSyncPulse() noexcept;
    void setTestTone(bool enabled, float freqHz, float gainDb) noexcept;
    int32_t runMicCalibration(const std::vector<std::wstring>& deviceIds, std::vector<WaCalibrationResult>& outResults);

    void getTelemetry(WaTelemetry* outTelemetry, bool includeSpectrum);
    void registerDeviceChangeCallback(WaDeviceChangeCallback cb, void* userContext);

private:
    void restoreOriginalDefaultIfNeeded();

    std::mutex controlMutex_;
    DeviceManager deviceManager_;
    dsp::MasterDspChain dspChain_;
    CaptureWorker captureWorker_;

    std::atomic<WaRoutingMode> routingMode_{ WA_ROUTING_STOPPED };
    int bufferMs_ = 15;

    // Saved original physical default device before switching Windows default to Virtual Sink
    std::wstring savedOriginalDefaultId_;
    std::wstring activeVirtualSinkId_;
    std::wstring activeLoopbackSourceId_;

    std::vector<WaOutputSlotConfig> lastSlotConfigs_;
    std::shared_ptr<std::vector<std::shared_ptr<OutputWorker>>> activeOutputs_;
};

} // namespace winaudio::audio
