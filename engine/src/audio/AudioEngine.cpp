#include "AudioEngine.h"
#include "PolicyConfig.h"
#include "AcousticCalibrator.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <algorithm>
#include <cstring>
#include <thread>

namespace winaudio::audio {

AudioEngine::AudioEngine()
    : captureWorker_(dspChain_) {
    deviceManager_.initialize();
    dspChain_.prepare(48000.0);
}

AudioEngine::~AudioEngine() {
    stop();
    deviceManager_.shutdown();
}

void AudioEngine::restoreOriginalDefaultIfNeeded() {
    if (!savedOriginalDefaultId_.empty()) {
        // Only restore if Windows is still pointing at our virtual sink
        const std::wstring curDefault = deviceManager_.getDefaultRenderDeviceId(eRender, eMultimedia);
        if (curDefault == activeVirtualSinkId_ || curDefault.empty()) {
            setSystemDefaultEndpoint(savedOriginalDefaultId_, true);
        }
        savedOriginalDefaultId_.clear();
    }
    activeVirtualSinkId_.clear();
    activeLoopbackSourceId_.clear();
}

bool AudioEngine::start(const std::wstring& preferredVirtualSinkId, bool preferVirtualMode, int bufferMs) {
    std::lock_guard<std::mutex> lock(controlMutex_);

    // Cleanly stop any prior session first
    captureWorker_.stop();
    if (activeOutputs_) {
        for (auto& w : *activeOutputs_) w->stop();
        activeOutputs_.reset();
    }
    restoreOriginalDefaultIfNeeded();

    bufferMs_ = std::clamp(bufferMs, 10, 60);

    std::wstring currentDefault = deviceManager_.getDefaultRenderDeviceId(eRender, eMultimedia);
    std::wstring virtualSinkId;
    if (preferVirtualMode) {
        virtualSinkId = deviceManager_.findVirtualSinkId(preferredVirtualSinkId);
    }

    if (!virtualSinkId.empty()) {
        // FULL SYSTEM DSP MODE via Virtual Audio Sink (e.g. VB-CABLE)
        // Save user's physical default endpoint so we ALWAYS restore it on stop/exit!
        if (!currentDefault.empty() && currentDefault != virtualSinkId) {
            savedOriginalDefaultId_ = currentDefault;
        }
        activeVirtualSinkId_ = virtualSinkId;

        // Route Windows default audio into the virtual sink
        setSystemDefaultEndpoint(virtualSinkId, true);

        // Prefer paired capture endpoint ("CABLE Output") if available; otherwise loopback on virtual render sink
        std::wstring pairedCapId = deviceManager_.findPairedVirtualCaptureId(virtualSinkId);
        if (!pairedCapId.empty()) {
            captureWorker_.start(pairedCapId, false, bufferMs_);
        } else {
            captureWorker_.start(virtualSinkId, true, bufferMs_);
        }

        routingMode_.store(WA_ROUTING_VIRTUAL_SINK, std::memory_order_release);

        // If no outputs were configured yet, default to sending processed audio to user's saved physical speaker!
        if (lastSlotConfigs_.empty() && !savedOriginalDefaultId_.empty()) {
            WaOutputSlotConfig defSlot{};
            size_t n = std::min(static_cast<size_t>(WA_DEVICE_ID_LEN - 1), savedOriginalDefaultId_.size());
            std::wmemcpy(defSlot.deviceId, savedOriginalDefaultId_.c_str(), n);
            defSlot.deviceId[n] = L'\0';
            defSlot.enabled = 1;
            defSlot.volume = 1.0f;
            defSlot.mute = 0;
            defSlot.balance = 0.0f;
            defSlot.syncDelayMs = 0.0f;
            defSlot.useCustomEq = 0;
            lastSlotConfigs_.push_back(defSlot);
        }
    } else {
        // ZERO-DRIVER DIRECT / LOOPBACK FALLBACK MODE
        // Captures current Windows default physical device via WASAPI Loopback and fans out to secondary devices
        activeLoopbackSourceId_ = currentDefault;
        captureWorker_.start(activeLoopbackSourceId_, true, bufferMs_);
        routingMode_.store(WA_ROUTING_DIRECT_LOOPBACK, std::memory_order_release);
    }

    // Re-apply configured output workers if any
    if (!lastSlotConfigs_.empty()) {
        auto configsCopy = lastSlotConfigs_;
        // Unlock-free helper via internal rebuild
        const double engRate = static_cast<double>(captureWorker_.sampleRate());
        auto newOutputs = std::make_shared<std::vector<std::shared_ptr<OutputWorker>>>();
        for (const auto& slot : configsCopy) {
            if (!slot.enabled || slot.deviceId[0] == L'\0') continue;
            std::wstring id(slot.deviceId);
            // Prevent feedback loop! Never render to the virtual sink or to the loopback source itself!
            if (!activeVirtualSinkId_.empty() && id == activeVirtualSinkId_) continue;
            if (routingMode_.load() == WA_ROUTING_DIRECT_LOOPBACK && id == activeLoopbackSourceId_) continue;

            auto worker = std::make_shared<OutputWorker>(slot, engRate, bufferMs_);
            worker->start();
            newOutputs->push_back(std::move(worker));
        }
        activeOutputs_ = newOutputs;
        captureWorker_.setOutputs(activeOutputs_);
    }

    return true;
}

void AudioEngine::stop() {
    std::lock_guard<std::mutex> lock(controlMutex_);

    captureWorker_.setOutputs(nullptr);
    captureWorker_.stop();

    if (activeOutputs_) {
        for (auto& w : *activeOutputs_) {
            w->stop();
        }
        activeOutputs_.reset();
    }

    restoreOriginalDefaultIfNeeded();
    routingMode_.store(WA_ROUTING_STOPPED, std::memory_order_release);
}

void AudioEngine::setMasterDsp(const WaDspParams& params) noexcept {
    dspChain_.setParams(params);
}

void AudioEngine::setBypass(bool bypass) noexcept {
    dspChain_.setBypass(bypass);
}

std::vector<WaDeviceInfo> AudioEngine::enumerateDevices() {
    return deviceManager_.enumerateRenderDevices();
}

bool AudioEngine::setDefaultDevice(const std::wstring& deviceId, bool includeCommunications) {
    if (deviceId.empty()) return false;
    bool ok = setSystemDefaultEndpoint(deviceId, includeCommunications);

    // If running in Direct Loopback mode and the user switched Windows default device,
    // cleanly re-anchor loopback capture to the new default device!
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (ok && routingMode_.load(std::memory_order_relaxed) == WA_ROUTING_DIRECT_LOOPBACK) {
        if (deviceId != activeLoopbackSourceId_) {
            activeLoopbackSourceId_ = deviceId;
            captureWorker_.start(activeLoopbackSourceId_, true, bufferMs_);
            // Rebuild outputs to exclude new loopback source and prevent echo/feedback
            const double engRate = static_cast<double>(captureWorker_.sampleRate());
            auto newOutputs = std::make_shared<std::vector<std::shared_ptr<OutputWorker>>>();
            for (const auto& slot : lastSlotConfigs_) {
                if (!slot.enabled || slot.deviceId[0] == L'\0') continue;
                std::wstring id(slot.deviceId);
                if (id == activeLoopbackSourceId_) continue;
                auto worker = std::make_shared<OutputWorker>(slot, engRate, bufferMs_);
                worker->start();
                newOutputs->push_back(std::move(worker));
            }
            auto old = activeOutputs_;
            activeOutputs_ = newOutputs;
            captureWorker_.setOutputs(activeOutputs_);
            if (old) {
                for (auto& w : *old) w->stop();
            }
        }
    } else if (ok && routingMode_.load(std::memory_order_relaxed) == WA_ROUTING_VIRTUAL_SINK) {
        // Update saved restore device if user explicitly switches physical default
        if (deviceId != activeVirtualSinkId_) {
            savedOriginalDefaultId_ = deviceId;
        }
    }
    return ok;
}

bool AudioEngine::configureOutputs(const WaOutputSlotConfig* slots, int count) {
    std::lock_guard<std::mutex> lock(controlMutex_);

    lastSlotConfigs_.clear();
    if (slots && count > 0) {
        int n = std::min(count, WA_MAX_OUTPUT_SLOTS);
        lastSlotConfigs_.assign(slots, slots + n);
    }

    if (routingMode_.load(std::memory_order_relaxed) == WA_ROUTING_STOPPED) {
        return true;
    }

    const double engRate = static_cast<double>(captureWorker_.sampleRate());
    auto newOutputs = std::make_shared<std::vector<std::shared_ptr<OutputWorker>>>();

    // Reuse already-running workers for unchanged devices so dragging volume/delay/EQ NEVER glitches audio!
    for (const auto& slot : lastSlotConfigs_) {
        if (!slot.enabled || slot.deviceId[0] == L'\0') continue;
        std::wstring id(slot.deviceId);

        // Feedback-loop guard
        if (!activeVirtualSinkId_.empty() && id == activeVirtualSinkId_) continue;
        if (routingMode_.load() == WA_ROUTING_DIRECT_LOOPBACK && id == activeLoopbackSourceId_) continue;

        std::shared_ptr<OutputWorker> existing;
        if (activeOutputs_) {
            for (const auto& w : *activeOutputs_) {
                if (w->deviceId() == id) {
                    existing = w;
                    break;
                }
            }
        }

        if (existing) {
            existing->updateConfig(slot);
            newOutputs->push_back(existing);
        } else {
            auto worker = std::make_shared<OutputWorker>(slot, engRate, bufferMs_);
            worker->start();
            newOutputs->push_back(std::move(worker));
        }
    }

    // Count total active Bluetooth endpoints (including BT default source in Direct Loopback mode).
    // When 3+ Bluetooth A2DP links share one PC radio, automatically engage High-Headroom BT buffering!
    auto allDevs = deviceManager_.enumerateRenderDevices();
    auto isBtId = [&](const std::wstring& devId) -> bool {
        for (const auto& d : allDevs) {
            if (devId == d.id) {
                return d.transport == WA_TRANSPORT_BLUETOOTH_A2DP || d.transport == WA_TRANSPORT_BLUETOOTH_HFP;
            }
        }
        return false;
    };

    int totalBtActive = 0;
    if (routingMode_.load() == WA_ROUTING_DIRECT_LOOPBACK && !activeLoopbackSourceId_.empty() && isBtId(activeLoopbackSourceId_)) {
        ++totalBtActive;
    }
    for (const auto& w : *newOutputs) {
        if (isBtId(w->deviceId())) ++totalBtActive;
    }
    const bool highHeadroomBt = (totalBtActive >= 3);
    for (const auto& w : *newOutputs) {
        w->setHighHeadroomMode(highHeadroomBt);
    }

    auto oldOutputs = activeOutputs_;
    activeOutputs_ = newOutputs;
    captureWorker_.setOutputs(activeOutputs_);

    // Cleanly stop any workers that were removed from the active list
    if (oldOutputs) {
        for (const auto& oldW : *oldOutputs) {
            bool stillActive = false;
            for (const auto& newW : *newOutputs) {
                if (newW.get() == oldW.get()) {
                    stillActive = true;
                    break;
                }
            }
            if (!stillActive) {
                oldW->stop();
            }
        }
    }

    return true;
}

void AudioEngine::triggerSyncPulse() noexcept {
    captureWorker_.triggerSyncPulse();
}

void AudioEngine::setTestTone(bool enabled, float freqHz, float gainDb) noexcept {
    captureWorker_.setTestTone(enabled, freqHz, gainDb);
}

namespace {

void playDirectChirpOnEndpoint(const std::wstring& deviceId) {
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didCom = SUCCEEDED(initHr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* mixFmt = nullptr;

    auto cleanup = [&]() {
        if (client) { client->Stop(); client->Release(); client = nullptr; }
        if (render) { render->Release(); render = nullptr; }
        if (mixFmt) { CoTaskMemFree(mixFmt); mixFmt = nullptr; }
        if (dev) { dev->Release(); dev = nullptr; }
        if (enumerator) { enumerator->Release(); enumerator = nullptr; }
    };

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator) {
        if (SUCCEEDED(enumerator->GetDevice(deviceId.c_str(), &dev)) && dev) {
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) && client) {
                if (SUCCEEDED(client->GetMixFormat(&mixFmt)) && mixFmt) {
                    const bool isFloat = (mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                        (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mixFmt->cbSize >= 22 &&
                         IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFmt)->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
                    if (isFloat &&
                        SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2500000LL, 0, mixFmt, nullptr)) &&
                        SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))) && render) {
                        const double sr = static_cast<double>(mixFmt->nSamplesPerSec);
                        const int ch = std::max(1, static_cast<int>(mixFmt->nChannels));
                        auto chirp = AcousticCalibrator::generateLogChirp(sr, 0.35f);
                        UINT32 bufFrames = 0;
                        client->GetBufferSize(&bufFrames);
                        const UINT32 nWrite = std::min<UINT32>(bufFrames, static_cast<UINT32>(chirp.size()));
                        BYTE* data = nullptr;
                        if (SUCCEEDED(render->GetBuffer(nWrite, &data)) && data) {
                            float* dst = reinterpret_cast<float*>(data);
                            for (UINT32 i = 0; i < nWrite; ++i) {
                                for (int c = 0; c < ch; ++c) dst[i * ch + c] = chirp[i];
                            }
                            render->ReleaseBuffer(nWrite, 0);
                            if (SUCCEEDED(client->Start())) {
                                Sleep(260);
                            }
                        }
                    }
                }
            }
        }
    }

    cleanup();
    if (didCom) CoUninitialize();
}

} // namespace

int32_t AudioEngine::runMicCalibration(const std::vector<std::wstring>& deviceIds, std::vector<WaCalibrationResult>& outResults) {
    outResults.clear();
    if (deviceIds.empty()) return 0;

    // Only proceed if an active physical microphone is attached!
    std::wstring micId, micName;
    if (!AcousticCalibrator::findActiveMicrophone(micId, micName)) {
        return 0;
    }

    std::shared_ptr<std::vector<std::shared_ptr<OutputWorker>>> workersSnap;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        workersSnap = activeOutputs_;
    }

    auto restoreWorkers = [&]() {
        if (!workersSnap) return;
        for (const auto& w : *workersSnap) {
            w->setCalibrationMute(false);
            w->setCalibrationBypassDelay(false);
        }
    };

    outResults.resize(deviceIds.size());
    for (size_t i = 0; i < deviceIds.size(); ++i) {
        std::memset(&outResults[i], 0, sizeof(WaCalibrationResult));
        size_t n = std::min(static_cast<size_t>(WA_DEVICE_ID_LEN - 1), deviceIds[i].size());
        std::wmemcpy(outResults[i].deviceId, deviceIds[i].c_str(), n);
        outResults[i].recommendedVolume = 1.0f;

        // Isolate this single output device during its ~950ms acoustic chirp measurement
        std::shared_ptr<OutputWorker> matchedWorker;
        if (workersSnap) {
            for (const auto& w : *workersSnap) {
                if (w->deviceId() == deviceIds[i]) {
                    matchedWorker = w;
                    w->setCalibrationMute(false);
                    w->setCalibrationBypassDelay(true);
                } else {
                    w->setCalibrationMute(true);
                }
            }
        }

        // Brief settle pause between speakers so room reverb decays cleanly
        Sleep(160);

        std::vector<float> micMono;
        double micSr = 48000.0;
        double triggerOffsetSec = 0.12;
        std::thread directPlayThread;

        bool captured = AcousticCalibrator::captureFromMicrophone(
            micId, 950, 120, micMono, micSr, triggerOffsetSec,
            [&]() {
                if (matchedWorker) {
                    matchedWorker->triggerCalibrationChirp();
                } else {
                    std::wstring idCopy = deviceIds[i];
                    directPlayThread = std::thread([idCopy]() { playDirectChirpOnEndpoint(idCopy); });
                }
            });

        if (directPlayThread.joinable()) {
            directPlayThread.join();
        }

        if (captured) {
            auto analysis = AcousticCalibrator::analyzeRecording(micMono, micSr, triggerOffsetSec, 650.0);
            float pipelineExtraMs = 0.0f;
            if (matchedWorker) {
                // Include this OutputWorker's live SPSC buffer fill so relative delay accounts for
                // differences between Wired (45ms SPSC), BT (95ms SPSC), and Direct Default (0ms SPSC)!
                pipelineExtraMs = matchedWorker->getTelemetry().bufferFillMs;
            }
            outResults[i].measuredLatencyMs = analysis.latencyMs + pipelineExtraMs;
            outResults[i].measuredLevelDb = analysis.rmsDb;
            outResults[i].confidence = analysis.confidence;
            outResults[i].succeeded = analysis.detected ? 1 : 0;
        }
    }

    restoreWorkers();
    AcousticCalibrator::computeRelativeAdjustments(outResults);
    return static_cast<int32_t>(outResults.size());
}

void AudioEngine::getTelemetry(WaTelemetry* outTelemetry, bool includeSpectrum) {
    if (!outTelemetry) return;
    std::memset(outTelemetry, 0, sizeof(WaTelemetry));

    outTelemetry->routingMode = static_cast<int32_t>(routingMode_.load(std::memory_order_relaxed));
    outTelemetry->isRunning = captureWorker_.isRunning() ? 1 : 0;
    outTelemetry->isBypassed = dspChain_.isBypassed() ? 1 : 0;
    outTelemetry->engineSampleRate = captureWorker_.sampleRate();
    outTelemetry->cpuLoadPercent = captureWorker_.cpuLoadPercent();

    const auto m = dspChain_.getMeters();
    outTelemetry->inputPeakDbL = m.inputPeakDbL;
    outTelemetry->inputPeakDbR = m.inputPeakDbR;
    outTelemetry->inputRmsDbL = m.inputRmsDbL;
    outTelemetry->inputRmsDbR = m.inputRmsDbR;
    outTelemetry->outputPeakDbL = m.outputPeakDbL;
    outTelemetry->outputPeakDbR = m.outputPeakDbR;
    outTelemetry->outputRmsDbL = m.outputRmsDbL;
    outTelemetry->outputRmsDbR = m.outputRmsDbR;
    outTelemetry->limiterGrDb = m.limiterGrDb;
    outTelemetry->loudnessGainDb = m.loudnessGainDb;

    if (includeSpectrum) {
        dspChain_.computeSpectrum(outTelemetry->spectrumDb);
    } else {
        for (int i = 0; i < WA_SPECTRUM_BINS; ++i) outTelemetry->spectrumDb[i] = -90.0f;
    }

    std::lock_guard<std::mutex> lock(controlMutex_);
    if (activeOutputs_) {
        int idx = 0;
        for (const auto& w : *activeOutputs_) {
            if (idx >= WA_MAX_OUTPUT_SLOTS) break;
            outTelemetry->outputs[idx++] = w->getTelemetry();
        }
        outTelemetry->activeOutputCount = idx;
    }
}

void AudioEngine::registerDeviceChangeCallback(WaDeviceChangeCallback cb, void* userContext) {
    if (!cb) {
        deviceManager_.setChangeCallback(nullptr);
    } else {
        deviceManager_.setChangeCallback([cb, userContext]() {
            cb(userContext);
        });
    }
}

} // namespace winaudio::audio
