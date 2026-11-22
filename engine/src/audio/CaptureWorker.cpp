#include "CaptureWorker.h"
#include "dsp/DspMath.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace winaudio::audio {

namespace {

bool isFloatFmt(const WAVEFORMATEX* fmt) noexcept {
    if (!fmt) return false;
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return false;
}

bool isPcm16Fmt(const WAVEFORMATEX* fmt) noexcept {
    if (!fmt) return false;
    if (fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->wBitsPerSample == 16) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22 && fmt->wBitsPerSample == 16) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != 0;
    }
    return false;
}

} // namespace

CaptureWorker::CaptureWorker(dsp::MasterDspChain& dspChain)
    : dsp_(dspChain) {}

CaptureWorker::~CaptureWorker() {
    stop();
}

void CaptureWorker::start(const std::wstring& captureEndpointId, bool isLoopback, int bufferMs) {
    stop();
    endpointId_ = captureEndpointId;
    isLoopback_ = isLoopback;
    bufferMs_ = std::clamp(bufferMs, 10, 60);
    stopRequested_.store(false, std::memory_order_release);
    thread_ = std::thread(&CaptureWorker::workerLoop, this);
}

void CaptureWorker::stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

void CaptureWorker::setOutputs(std::shared_ptr<std::vector<std::shared_ptr<OutputWorker>>> outputs) {
    const int32_t curSr = sampleRate_.load(std::memory_order_acquire);
    if (outputs) {
        for (const auto& w : *outputs) {
            w->setInputSampleRate(curSr);
        }
    }
    {
        std::lock_guard<std::mutex> lock(outputsMutex_);
        outputs_ = std::move(outputs);
    }
    outputsVersion_.fetch_add(1, std::memory_order_release);
}

void CaptureWorker::triggerSyncPulse() noexcept {
    int sr = sampleRate_.load(std::memory_order_relaxed);
    syncPulseSamplesRemaining_.store(static_cast<int>(sr * 0.035), std::memory_order_relaxed);
    syncPulsePhase_ = 0.0;
}

void CaptureWorker::setTestTone(bool enabled, float freqHz, float gainDb) noexcept {
    testToneFreq_.store(dsp::clampf(freqHz, 20.0f, 18000.0f), std::memory_order_relaxed);
    testToneGain_.store(dsp::dbToLinear(dsp::clampf(gainDb, -48.0f, -1.0f)), std::memory_order_relaxed);
    testToneEnabled_.store(enabled, std::memory_order_relaxed);
}

void CaptureWorker::injectTestAndPulse(float* interleavedStereo, int numFrames, double sr) noexcept {
    const bool toneOn = testToneEnabled_.load(std::memory_order_relaxed);
    int pulseLeft = syncPulseSamplesRemaining_.load(std::memory_order_relaxed);

    if (!toneOn && pulseLeft <= 0) return;

    const double toneStep = dsp::kTwoPi * static_cast<double>(testToneFreq_.load(std::memory_order_relaxed)) / sr;
    const float toneGain = testToneGain_.load(std::memory_order_relaxed);
    const double pulseStep = dsp::kTwoPi * 1000.0 / sr;
    const int pulseTotal = std::max(1, static_cast<int>(sr * 0.035));

    for (int i = 0; i < numFrames; ++i) {
        float add = 0.0f;
        if (toneOn) {
            add += static_cast<float>(std::sin(testTonePhase_)) * toneGain;
            testTonePhase_ += toneStep;
            if (testTonePhase_ >= dsp::kTwoPi) testTonePhase_ -= dsp::kTwoPi;
        }
        if (pulseLeft > 0) {
            double env = std::sin(dsp::kPi * static_cast<double>(pulseLeft) / static_cast<double>(pulseTotal));
            add += static_cast<float>(std::sin(syncPulsePhase_) * env * 0.45);
            syncPulsePhase_ += pulseStep;
            if (syncPulsePhase_ >= dsp::kTwoPi) syncPulsePhase_ -= dsp::kTwoPi;
            --pulseLeft;
        }
        interleavedStereo[i * 2] += add;
        interleavedStereo[i * 2 + 1] += add;
    }

    if (pulseLeft != syncPulseSamplesRemaining_.load(std::memory_order_relaxed)) {
        syncPulseSamplesRemaining_.store(pulseLeft, std::memory_order_relaxed);
    }
}

void CaptureWorker::workerLoop() {
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didCom = SUCCEEDED(comHr);

    // Use MMCSS "Pro Audio" WITHOUT raw THREAD_PRIORITY_TIME_CRITICAL so Windows audiodg.exe
    // and Bluetooth kernel drivers (BthPort.sys) are never starved when multiple outputs run!
    DWORD mmcssTaskIdx = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIdx);

    // High-resolution waitable timer (~0.5ms accuracy vs default 15.6ms Sleep()!)
    HANDLE hiResTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!hiResTimer) {
        hiResTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    auto sleepHiResMs = [&](int ms) {
        if (hiResTimer) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(ms) * 10000LL;
            if (SetWaitableTimer(hiResTimer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(hiResTimer, static_cast<DWORD>(ms + 20));
                return;
            }
        }
        Sleep(static_cast<DWORD>(ms));
    };

    std::vector<float> stereoBlock(4096 * 2, 0.0f);
    std::shared_ptr<std::vector<std::shared_ptr<OutputWorker>>> cachedOutputs;
    uint64_t cachedOutputsVer = 0;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* capClient = nullptr;
        IAudioCaptureClient* captureSvc = nullptr;
        IAudioClient* keepAliveClient = nullptr;
        IAudioRenderClient* keepAliveRender = nullptr;
        WAVEFORMATEX* mixFmt = nullptr;
        HANDLE capEvent = nullptr;

        auto cleanup = [&]() {
            if (capClient) { capClient->Stop(); capClient->Release(); capClient = nullptr; }
            if (captureSvc) { captureSvc->Release(); captureSvc = nullptr; }
            if (keepAliveClient) { keepAliveClient->Stop(); keepAliveClient->Release(); keepAliveClient = nullptr; }
            if (keepAliveRender) { keepAliveRender->Release(); keepAliveRender = nullptr; }
            if (mixFmt) { CoTaskMemFree(mixFmt); mixFmt = nullptr; }
            if (device) { device->Release(); device = nullptr; }
            if (enumerator) { enumerator->Release(); enumerator = nullptr; }
            if (capEvent) { CloseHandle(capEvent); capEvent = nullptr; }
        };

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) {
            cleanup();
            Sleep(500);
            continue;
        }

        if (!endpointId_.empty()) {
            hr = enumerator->GetDevice(endpointId_.c_str(), &device);
        } else {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        }
        if (FAILED(hr) || !device) {
            cleanup();
            Sleep(500);
            continue;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&capClient));
        if (FAILED(hr) || !capClient) {
            cleanup();
            Sleep(500);
            continue;
        }

        hr = capClient->GetMixFormat(&mixFmt);
        if (FAILED(hr) || !mixFmt) {
            cleanup();
            Sleep(500);
            continue;
        }

        const bool useFloat = isFloatFmt(mixFmt);
        const bool usePcm16 = !useFloat && isPcm16Fmt(mixFmt);
        if (!useFloat && !usePcm16) {
            cleanup();
            Sleep(1000);
            continue;
        }

        // For WASAPI Loopback, open a silent companion render client on the same endpoint
        // so Windows keeps delivering loopback packets smoothly even during brief silence.
        if (isLoopback_) {
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&keepAliveClient))) && keepAliveClient) {
                if (SUCCEEDED(keepAliveClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000LL, 0, mixFmt, nullptr))) {
                    if (SUCCEEDED(keepAliveClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&keepAliveRender))) && keepAliveRender) {
                        UINT32 kaFrames = 0;
                        keepAliveClient->GetBufferSize(&kaFrames);
                        BYTE* kaBuf = nullptr;
                        if (SUCCEEDED(keepAliveRender->GetBuffer(kaFrames, &kaBuf)) && kaBuf) {
                            std::memset(kaBuf, 0, static_cast<size_t>(kaFrames) * mixFmt->nBlockAlign);
                            keepAliveRender->ReleaseBuffer(kaFrames, AUDCLNT_BUFFERFLAGS_SILENT);
                        }
                        keepAliveClient->Start();
                    }
                }
            }
        }

        // Use a generous 100ms capture ring for loopback so OS thread scheduling never drops packets
        const REFERENCE_TIME hnsDuration = isLoopback_
            ? 1000000LL
            : (static_cast<REFERENCE_TIME>(std::max(20, bufferMs_ * 2)) * 10000LL);
        DWORD flags = isLoopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;

        bool eventDriven = false;
        if (!isLoopback_) {
            capEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            hr = capClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsDuration, 0, mixFmt, nullptr);
            if (SUCCEEDED(hr)) {
                capClient->SetEventHandle(capEvent);
                eventDriven = true;
            }
        }
        if (!eventDriven) {
            hr = capClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, hnsDuration, 0, mixFmt, nullptr);
        }
        if (FAILED(hr)) {
            cleanup();
            Sleep(500);
            continue;
        }

        hr = capClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureSvc));
        if (FAILED(hr) || !captureSvc) {
            cleanup();
            Sleep(500);
            continue;
        }

        const double sr = static_cast<double>(mixFmt->nSamplesPerSec);
        const int32_t srInt = static_cast<int32_t>(mixFmt->nSamplesPerSec);
        const int inCh = static_cast<int>(mixFmt->nChannels);
        sampleRate_.store(srInt, std::memory_order_release);
        dsp_.prepare(sr);

        // Immediately propagate exact capture sample rate to any already-running OutputWorkers
        {
            std::lock_guard<std::mutex> lock(outputsMutex_);
            if (outputs_) {
                for (const auto& w : *outputs_) w->setInputSampleRate(srInt);
            }
        }

        hr = capClient->Start();
        if (FAILED(hr)) {
            cleanup();
            Sleep(500);
            continue;
        }

        running_.store(true, std::memory_order_release);
        float smoothedCpu = 0.0f;

        while (!stopRequested_.load(std::memory_order_acquire)) {
            if (eventDriven && capEvent) {
                WaitForSingleObject(capEvent, 50);
            } else {
                sleepHiResMs(4); // Crisp 4ms loopback drain cadence (immune to 15.6ms OS tick!)
            }

            // Keep silent loopback pump alive if needed
            if (keepAliveClient && keepAliveRender) {
                UINT32 kaBufSz = 0, kaPad = 0;
                if (SUCCEEDED(keepAliveClient->GetBufferSize(&kaBufSz)) && SUCCEEDED(keepAliveClient->GetCurrentPadding(&kaPad))) {
                    UINT32 kaAvail = (kaBufSz > kaPad) ? (kaBufSz - kaPad) : 0;
                    if (kaAvail > 0) {
                        BYTE* kaBuf = nullptr;
                        if (SUCCEEDED(keepAliveRender->GetBuffer(kaAvail, &kaBuf)) && kaBuf) {
                            std::memset(kaBuf, 0, static_cast<size_t>(kaAvail) * mixFmt->nBlockAlign);
                            keepAliveRender->ReleaseBuffer(kaAvail, AUDCLNT_BUFFERFLAGS_SILENT);
                        }
                    }
                }
            }

            UINT32 packetLength = 0;
            hr = captureSvc->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;

            while (packetLength > 0 && !stopRequested_.load(std::memory_order_acquire)) {
                BYTE* pData = nullptr;
                UINT32 numFrames = 0;
                DWORD pktFlags = 0;

                hr = captureSvc->GetBuffer(&pData, &numFrames, &pktFlags, nullptr, nullptr);
                if (FAILED(hr)) break;

                if (numFrames > 0) {
                    if (stereoBlock.size() < static_cast<size_t>(numFrames) * 2) {
                        stereoBlock.resize(static_cast<size_t>(numFrames) * 2, 0.0f);
                    }

                    if ((pktFlags & AUDCLNT_BUFFERFLAGS_SILENT) || !pData) {
                        std::fill(stereoBlock.begin(), stereoBlock.begin() + static_cast<size_t>(numFrames) * 2, 0.0f);
                    } else if (useFloat) {
                        const float* src = reinterpret_cast<const float*>(pData);
                        if (inCh == 2) {
                            std::memcpy(stereoBlock.data(), src, static_cast<size_t>(numFrames) * 2 * sizeof(float));
                        } else if (inCh == 1) {
                            for (UINT32 i = 0; i < numFrames; ++i) {
                                stereoBlock[i * 2] = src[i];
                                stereoBlock[i * 2 + 1] = src[i];
                            }
                        } else {
                            // Downmix multichannel (e.g. 5.1/7.1) front L/R + center cleanly to stereo
                            for (UINT32 i = 0; i < numFrames; ++i) {
                                const float* f = src + i * inCh;
                                float c = (inCh >= 3) ? f[2] * 0.707f : 0.0f;
                                stereoBlock[i * 2] = f[0] + c;
                                stereoBlock[i * 2 + 1] = f[1] + c;
                            }
                        }
                    } else if (usePcm16) {
                        const int16_t* src = reinterpret_cast<const int16_t*>(pData);
                        constexpr float inv32768 = 1.0f / 32768.0f;
                        for (UINT32 i = 0; i < numFrames; ++i) {
                            if (inCh == 1) {
                                float v = src[i] * inv32768;
                                stereoBlock[i * 2] = v;
                                stereoBlock[i * 2 + 1] = v;
                            } else {
                                stereoBlock[i * 2] = src[i * inCh] * inv32768;
                                stereoBlock[i * 2 + 1] = src[i * inCh + 1] * inv32768;
                            }
                        }
                    }

                    const auto t0 = std::chrono::steady_clock::now();

                    injectTestAndPulse(stereoBlock.data(), static_cast<int>(numFrames), sr);
                    dsp_.processBlock(stereoBlock.data(), static_cast<int>(numFrames));

                    // Lock-free fan-out: only refresh cached output list when configuration actually changes!
                    const uint64_t curVer = outputsVersion_.load(std::memory_order_acquire);
                    if (curVer != cachedOutputsVer) {
                        std::lock_guard<std::mutex> lock(outputsMutex_);
                        cachedOutputs = outputs_;
                        cachedOutputsVer = curVer;
                    }
                    if (cachedOutputs) {
                        for (const auto& worker : *cachedOutputs) {
                            worker->pushAudio(stereoBlock.data(), numFrames, srInt);
                        }
                    }

                    const auto t1 = std::chrono::steady_clock::now();
                    const double elapsedUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
                    const double budgetUs = (static_cast<double>(numFrames) / sr) * 1e6;
                    const float instCpu = static_cast<float>(std::clamp((elapsedUs / std::max(1.0, budgetUs)) * 100.0, 0.0, 100.0));
                    smoothedCpu = smoothedCpu * 0.9f + instCpu * 0.1f;
                    cpuLoadPercent_.store(smoothedCpu, std::memory_order_relaxed);
                }

                captureSvc->ReleaseBuffer(numFrames);
                hr = captureSvc->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) break;
            }
        }

        running_.store(false, std::memory_order_release);
        cleanup();

        if (!stopRequested_.load(std::memory_order_acquire)) {
            Sleep(400);
        }
    }

    if (hiResTimer) CloseHandle(hiResTimer);
    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
    if (didCom) CoUninitialize();
}

} // namespace winaudio::audio
