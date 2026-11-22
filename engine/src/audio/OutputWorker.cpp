#include "OutputWorker.h"
#include "AcousticCalibrator.h"
#include "dsp/DspMath.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <vector>

namespace winaudio::audio {

namespace {

static const PROPERTYKEY kOutPkeyEnumerator = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 24
};
static const PROPERTYKEY kOutPkeyInstanceId = {
    { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57 } }, 256
};

bool containsCI(std::wstring hay, const wchar_t* needle) {
    std::transform(hay.begin(), hay.end(), hay.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::wstring n(needle);
    std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return hay.find(n) != std::wstring::npos;
}

bool detectBluetoothEndpoint(IMMDevice* device) {
    if (!device) return false;
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store) return false;

    auto readStr = [&](const PROPERTYKEY& k) -> std::wstring {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        std::wstring s;
        if (SUCCEEDED(store->GetValue(k, &pv))) {
            if (pv.vt == VT_LPWSTR && pv.pwszVal) s = pv.pwszVal;
            PropVariantClear(&pv);
        }
        return s;
    };

    const std::wstring meta = readStr(kOutPkeyEnumerator) + L" " +
                              readStr(kOutPkeyInstanceId) + L" " +
                              readStr(PKEY_Device_FriendlyName) + L" " +
                              readStr(PKEY_DeviceInterface_FriendlyName);
    store->Release();

    return containsCI(meta, L"bthenum") ||
           containsCI(meta, L"bthhfenum") ||
           containsCI(meta, L"btha2dp") ||
           containsCI(meta, L"bluetooth") ||
           containsCI(meta, L"a2dp") ||
           containsCI(meta, L"hands-free");
}

bool isFloatFormat(const WAVEFORMATEX* fmt) noexcept {
    if (!fmt) return false;
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return false;
}

bool isPcm16Format(const WAVEFORMATEX* fmt) noexcept {
    if (!fmt) return false;
    if (fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->wBitsPerSample == 16) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22 && fmt->wBitsPerSample == 16) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != 0;
    }
    return false;
}

} // namespace

OutputWorker::OutputWorker(const WaOutputSlotConfig& cfg, double engineSampleRate, int bufferMs)
    : deviceId_(cfg.deviceId)
    , inputSampleRate_(static_cast<int32_t>(std::max(8000.0, engineSampleRate)))
    , bufferMs_(std::clamp(bufferMs, 10, 60))
    , ring_(65536) {
    updateConfig(cfg);
}

OutputWorker::~OutputWorker() {
    stop();
}

void OutputWorker::start() {
    if (thread_.joinable()) return;
    stopRequested_.store(false, std::memory_order_release);
    ring_.reset();
    thread_ = std::thread(&OutputWorker::workerLoop, this);
}

void OutputWorker::stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
    state_.store(WA_OUTPUT_IDLE, std::memory_order_relaxed);
}

void OutputWorker::updateConfig(const WaOutputSlotConfig& cfg) {
    volume_.store(dsp::clampf(cfg.volume, 0.0f, 1.5f), std::memory_order_relaxed);
    mute_.store(cfg.mute != 0, std::memory_order_relaxed);
    balance_.store(dsp::clampf(cfg.balance, -1.0f, 1.0f), std::memory_order_relaxed);
    syncDelayMs_.store(dsp::clampf(cfg.syncDelayMs, 0.0f, 500.0f), std::memory_order_relaxed);
    useCustomEq_.store(cfg.useCustomEq != 0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(eqMutex_);
        pendingEq_ = cfg.customEq;
    }
    eqSeq_.fetch_add(1, std::memory_order_release);
}

WaOutputTelemetry OutputWorker::getTelemetry() const noexcept {
    WaOutputTelemetry t{};
    size_t n = std::min(static_cast<size_t>(WA_DEVICE_ID_LEN - 1), deviceId_.size());
    std::wmemcpy(t.deviceId, deviceId_.c_str(), n);
    t.deviceId[n] = L'\0';

    t.state = state_.load(std::memory_order_relaxed);
    t.sampleRate = endpointSampleRate_.load(std::memory_order_relaxed);
    t.bufferFillMs = bufferFillMs_.load(std::memory_order_relaxed);
    t.driftPpm = driftPpm_.load(std::memory_order_relaxed);
    t.appliedDelayMs = appliedDelayMs_.load(std::memory_order_relaxed);
    t.underruns = underruns_.load(std::memory_order_relaxed);
    t.overruns = overruns_.load(std::memory_order_relaxed);
    return t;
}

void OutputWorker::workerLoop() {
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didCom = SUCCEEDED(comHr);

    // Use MMCSS "Pro Audio" WITHOUT raw THREAD_PRIORITY_TIME_CRITICAL so multiple output workers
    // never starve CaptureWorker, audiodg.exe, or the Windows Bluetooth radio kernel driver!
    DWORD mmcssTaskIdx = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIdx);

    // High-resolution waitable timer (~0.5ms precision for Bluetooth & polling fallback)
    HANDLE hiResTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!hiResTimer) {
        hiResTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    auto sleepHiResMs = [&](int ms) {
        if (hiResTimer) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(ms) * 10000LL;
            if (SetWaitableTimer(hiResTimer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(hiResTimer, static_cast<DWORD>(ms + 25));
                return;
            }
        }
        Sleep(static_cast<DWORD>(ms));
    };

    std::vector<float> scratchStereo(8192 * 2, 0.0f);
    dsp::OnePoleSmoother gainSmootherL, gainSmootherR;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        state_.store(WA_OUTPUT_RECONNECTING, std::memory_order_relaxed);

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* audioClient = nullptr;
        IAudioRenderClient* renderClient = nullptr;
        WAVEFORMATEX* mixFmt = nullptr;
        HANDLE readyEvent = nullptr;

        auto cleanupSession = [&]() {
            if (audioClient) { audioClient->Stop(); audioClient->Release(); audioClient = nullptr; }
            if (renderClient) { renderClient->Release(); renderClient = nullptr; }
            if (mixFmt) { CoTaskMemFree(mixFmt); mixFmt = nullptr; }
            if (device) { device->Release(); device = nullptr; }
            if (enumerator) { enumerator->Release(); enumerator = nullptr; }
            if (readyEvent) { CloseHandle(readyEvent); readyEvent = nullptr; }
        };

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) {
            cleanupSession();
            Sleep(500);
            continue;
        }

        hr = enumerator->GetDevice(deviceId_.c_str(), &device);
        if (FAILED(hr) || !device) {
            cleanupSession();
            Sleep(750);
            continue;
        }

        DWORD devState = 0;
        if (FAILED(device->GetState(&devState)) || devState != DEVICE_STATE_ACTIVE) {
            cleanupSession();
            Sleep(750);
            continue;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
        if (FAILED(hr) || !audioClient) {
            cleanupSession();
            Sleep(500);
            continue;
        }

        hr = audioClient->GetMixFormat(&mixFmt);
        if (FAILED(hr) || !mixFmt) {
            cleanupSession();
            Sleep(500);
            continue;
        }

        const bool useFloat = isFloatFormat(mixFmt);
        const bool usePcm16 = !useFloat && isPcm16Format(mixFmt);
        if (!useFloat && !usePcm16) {
            state_.store(WA_OUTPUT_ERROR, std::memory_order_relaxed);
            cleanupSession();
            Sleep(1000);
            continue;
        }

        const bool isBluetooth = detectBluetoothEndpoint(device);
        isBluetooth_.store(isBluetooth, std::memory_order_relaxed);

        // Allocate a generous 240ms WASAPI render buffer capacity on Bluetooth (zero extra latency
        // when padding is bounded to 55ms for 1-2 BT devices, and lets 3+ BT High-Headroom mode
        // smoothly expand padding up to 175ms on-the-fly without restarting audio!).
        const int renderBufMs = isBluetooth
            ? std::max(240, bufferMs_ * 6)
            : std::max(60, bufferMs_ * 3);
        const REFERENCE_TIME hnsDuration = static_cast<REFERENCE_TIME>(renderBufMs) * 10000LL;

        bool eventDriven = false;
        if (!isBluetooth) {
            readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsDuration, 0, mixFmt, nullptr);
            eventDriven = SUCCEEDED(hr);
        }
        if (!eventDriven) {
            // Smooth high-resolution timer-paced shared mode (rock-solid on multi-Bluetooth A2DP!)
            hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsDuration, 0, mixFmt, nullptr);
        }
        if (FAILED(hr)) {
            state_.store(WA_OUTPUT_ERROR, std::memory_order_relaxed);
            cleanupSession();
            Sleep(750);
            continue;
        }

        if (eventDriven && readyEvent) {
            audioClient->SetEventHandle(readyEvent);
        }

        UINT32 bufferFrames = 0;
        audioClient->GetBufferSize(&bufferFrames);
        hr = audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient));
        if (FAILED(hr) || !renderClient) {
            cleanupSession();
            Sleep(500);
            continue;
        }

        const double outRate = static_cast<double>(mixFmt->nSamplesPerSec);
        const int outChannels = static_cast<int>(mixFmt->nChannels);
        endpointSampleRate_.store(static_cast<int32_t>(mixFmt->nSamplesPerSec), std::memory_order_relaxed);

        double adaptivePadBoostMs = 0.0;
        double adaptiveFillBoostMs = 0.0;

        auto calcTargetFillMs = [&](bool highHeadroom) -> double {
            if (isBluetooth) {
                double base = highHeadroom
                    ? std::max(145.0, static_cast<double>(bufferMs_) * 4.5)
                    : std::max(95.0, static_cast<double>(bufferMs_) * 3.5);
                return std::min(220.0, base + adaptiveFillBoostMs);
            }
            return std::max(45.0, static_cast<double>(bufferMs_) * 2.0);
        };

        auto calcTargetPaddingFrames = [&](bool highHeadroom) -> UINT32 {
            double padMs = isBluetooth
                ? std::min(200.0, (highHeadroom ? std::max(175.0, static_cast<double>(bufferMs_) * 4.5) : std::max(80.0, static_cast<double>(bufferMs_) * 2.8)) + adaptivePadBoostMs)
                : std::max(25.0, static_cast<double>(bufferMs_) * 1.5);
            return std::clamp(
                static_cast<UINT32>(std::round((padMs * 0.001) * outRate)),
                static_cast<UINT32>(outRate * 0.015),
                bufferFrames);
        };

        bool activeHighHeadroom = highHeadroomMode_.load(std::memory_order_acquire);
        double targetFillMs = calcTargetFillMs(activeHighHeadroom);
        int32_t activeInRate = inputSampleRate_.load(std::memory_order_acquire);
        resampler_.prepare(static_cast<double>(activeInRate), outRate, targetFillMs);
        resampler_.setDelayMs(syncDelayMs_.load(std::memory_order_relaxed));

        customEq_.prepare(outRate);
        uint32_t appliedEqSeq = 0;

        gainSmootherL.prepare(outRate, 12.0, 1.0f);
        gainSmootherR.prepare(outRate, 12.0, 1.0f);

        if (scratchStereo.size() < static_cast<size_t>(bufferFrames) * 2) {
            scratchStereo.resize(static_cast<size_t>(bufferFrames) * 2, 0.0f);
        }

        UINT32 targetPaddingFrames = calcTargetPaddingFrames(activeHighHeadroom);

        // Pre-fill only targetPaddingFrames with explicit zeroed silence before Start()
        BYTE* initData = nullptr;
        if (SUCCEEDED(renderClient->GetBuffer(targetPaddingFrames, &initData)) && initData) {
            std::memset(initData, 0, static_cast<size_t>(targetPaddingFrames) * mixFmt->nBlockAlign);
            renderClient->ReleaseBuffer(targetPaddingFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        ring_.reset();
        hr = audioClient->Start();
        if (FAILED(hr)) {
            cleanupSession();
            Sleep(500);
            continue;
        }

        running_.store(true, std::memory_order_release);
        state_.store(WA_OUTPUT_RUNNING, std::memory_order_relaxed);

        UINT32 lastUnderruns = 0;
        const UINT32 lowPadThreshold = static_cast<UINT32>(outRate * 0.020); // 20 ms hardware danger zone
        const auto calibChirp = AcousticCalibrator::generateLogChirp(outRate, 0.35f);
        size_t calibChirpPos = calibChirp.size();

        while (!stopRequested_.load(std::memory_order_acquire)) {
            if (eventDriven && readyEvent) {
                DWORD waitRes = WaitForSingleObject(readyEvent, 100);
                if (waitRes == WAIT_FAILED) break;
            } else {
                sleepHiResMs(isBluetooth ? (activeHighHeadroom ? 14 : 10) : 6);
            }

            // Dynamically adapt padding & SPSC cushion when 3+ Bluetooth devices are active!
            const bool wantHighHeadroom = highHeadroomMode_.load(std::memory_order_acquire);
            if (wantHighHeadroom != activeHighHeadroom) {
                activeHighHeadroom = wantHighHeadroom;
                targetFillMs = calcTargetFillMs(activeHighHeadroom);
                targetPaddingFrames = calcTargetPaddingFrames(activeHighHeadroom);
                resampler_.setTargetBufferMs(targetFillMs);
            }

            UINT32 padding = 0;
            hr = audioClient->GetCurrentPadding(&padding);
            if (FAILED(hr)) break; // Device invalidated / disconnected!

            // Self-healing per-device Bluetooth jitter escalation: if hardware padding dips < 20ms
            // (e.g. Echo speaker Wi-Fi/BT antenna switch burst), automatically expand cushion!
            if (isBluetooth && padding < lowPadThreshold && adaptivePadBoostMs < 110.0) {
                adaptivePadBoostMs = std::min(110.0, adaptivePadBoostMs + 25.0);
                adaptiveFillBoostMs = std::min(90.0, adaptiveFillBoostMs + 20.0);
                targetFillMs = calcTargetFillMs(activeHighHeadroom);
                targetPaddingFrames = calcTargetPaddingFrames(activeHighHeadroom);
                resampler_.setTargetBufferMs(targetFillMs);
            }

            // Write only enough frames to maintain targetPaddingFrames (keeps BT latency ~85ms lower!)
            UINT32 maxRoom = (bufferFrames > padding) ? (bufferFrames - padding) : 0;
            UINT32 desired = (targetPaddingFrames > padding) ? (targetPaddingFrames - padding) : 0;
            UINT32 framesAvail = eventDriven ? maxRoom : std::min(maxRoom, desired);
            if (framesAvail == 0) continue;

            // Dynamically adapt if CaptureWorker's live WASAPI sample rate changed (e.g. 48k -> 44.1k BT or 192k Digital!)
            const int32_t latestInRate = inputSampleRate_.load(std::memory_order_acquire);
            if (latestInRate != activeInRate && latestInRate >= 8000) {
                activeInRate = latestInRate;
                ring_.reset();
                resampler_.prepare(static_cast<double>(activeInRate), outRate, targetFillMs);
            }

            // Update dynamic per-device parameters (zero delay temporarily while calibrating)
            const bool bypassDelay = calibrationBypassDelay_.load(std::memory_order_acquire);
            resampler_.setDelayMs(bypassDelay ? 0.0f : syncDelayMs_.load(std::memory_order_relaxed));

            if (calibrationChirpTrigger_.exchange(false, std::memory_order_acq_rel)) {
                calibChirpPos = 0;
            }

            const uint32_t curEqSeq = eqSeq_.load(std::memory_order_acquire);
            if (curEqSeq != appliedEqSeq) {
                WaEqParams eqCopy{};
                {
                    std::lock_guard<std::mutex> lock(eqMutex_);
                    eqCopy = pendingEq_;
                }
                customEq_.updateParams(eqCopy);
                appliedEqSeq = curEqSeq;
            }

            const bool isCalibMuted = calibrationMute_.load(std::memory_order_acquire);
            const bool isMuted = isCalibMuted || mute_.load(std::memory_order_relaxed);
            const float vol = isMuted ? 0.0f : volume_.load(std::memory_order_relaxed);
            const float bal = balance_.load(std::memory_order_relaxed);
            const float targetL = vol * std::min(1.0f, 1.0f - bal);
            const float targetR = vol * std::min(1.0f, 1.0f + bal);
            gainSmootherL.setTarget(targetL);
            gainSmootherR.setTarget(targetR);

            // Pull resampled, drift-compensated, sync-delayed stereo frames
            resampler_.pull(ring_, scratchStereo.data(), static_cast<int>(framesAvail));

            // If a calibration chirp is playing on this worker, replace block cleanly with the chirp
            const bool playingCalibChirp = (calibChirpPos < calibChirp.size());
            if (playingCalibChirp) {
                for (UINT32 i = 0; i < framesAvail; ++i) {
                    float c = (calibChirpPos < calibChirp.size()) ? calibChirp[calibChirpPos++] : 0.0f;
                    scratchStereo[i * 2] = c;
                    scratchStereo[i * 2 + 1] = c;
                }
            }

            const bool applyEq = !playingCalibChirp && useCustomEq_.load(std::memory_order_relaxed);

            BYTE* outBuf = nullptr;
            hr = renderClient->GetBuffer(framesAvail, &outBuf);
            if (FAILED(hr) || !outBuf) break;

            if (useFloat) {
                float* dst = reinterpret_cast<float*>(outBuf);
                for (UINT32 i = 0; i < framesAvail; ++i) {
                    double l = scratchStereo[i * 2];
                    double r = scratchStereo[i * 2 + 1];
                    if (applyEq) customEq_.processSample(l, r);

                    const float gL = playingCalibChirp ? 1.0f : gainSmootherL.next();
                    const float gR = playingCalibChirp ? 1.0f : gainSmootherR.next();
                    const float fl = dsp::clampf(static_cast<float>(l) * gL, -1.0f, 1.0f);
                    const float fr = dsp::clampf(static_cast<float>(r) * gR, -1.0f, 1.0f);

                    if (outChannels == 2) {
                        dst[i * 2] = fl;
                        dst[i * 2 + 1] = fr;
                    } else if (outChannels == 1) {
                        dst[i] = 0.5f * (fl + fr);
                    } else {
                        float* frame = dst + i * outChannels;
                        std::memset(frame, 0, static_cast<size_t>(outChannels) * sizeof(float));
                        frame[0] = fl;
                        frame[1] = fr;
                    }
                }
            } else if (usePcm16) {
                int16_t* dst = reinterpret_cast<int16_t*>(outBuf);
                for (UINT32 i = 0; i < framesAvail; ++i) {
                    double l = scratchStereo[i * 2];
                    double r = scratchStereo[i * 2 + 1];
                    if (applyEq) customEq_.processSample(l, r);

                    const float fl = dsp::clampf(static_cast<float>(l) * gainSmootherL.next(), -1.0f, 1.0f);
                    const float fr = dsp::clampf(static_cast<float>(r) * gainSmootherR.next(), -1.0f, 1.0f);

                    auto toS16 = [](float x) noexcept {
                        return static_cast<int16_t>(std::lround(dsp::clampf(x, -1.0f, 1.0f) * 32767.0f));
                    };

                    if (outChannels == 2) {
                        dst[i * 2] = toS16(fl);
                        dst[i * 2 + 1] = toS16(fr);
                    } else if (outChannels == 1) {
                        dst[i] = toS16(0.5f * (fl + fr));
                    } else {
                        int16_t* frame = dst + i * outChannels;
                        std::memset(frame, 0, static_cast<size_t>(outChannels) * sizeof(int16_t));
                        frame[0] = toS16(fl);
                        frame[1] = toS16(fr);
                    }
                }
            }

            hr = renderClient->ReleaseBuffer(framesAvail, 0);
            if (FAILED(hr)) break;

            // Publish lock-free telemetry & escalate SPSC cushion if ring underrun occurred
            const uint32_t curUnderruns = resampler_.underrunCount();
            if (isBluetooth && curUnderruns > lastUnderruns && adaptiveFillBoostMs < 110.0) {
                lastUnderruns = curUnderruns;
                adaptiveFillBoostMs = std::min(110.0, adaptiveFillBoostMs + 25.0);
                targetFillMs = calcTargetFillMs(activeHighHeadroom);
                resampler_.setTargetBufferMs(targetFillMs);
            }
            bufferFillMs_.store(resampler_.smoothedFillMs(), std::memory_order_relaxed);
            driftPpm_.store(resampler_.driftPpm(), std::memory_order_relaxed);
            appliedDelayMs_.store(static_cast<float>(resampler_.appliedDelayMs()), std::memory_order_relaxed);
            underruns_.store(curUnderruns, std::memory_order_relaxed);
            overruns_.store(resampler_.overrunCount(), std::memory_order_relaxed);
        }

        running_.store(false, std::memory_order_release);
        cleanupSession();

        if (!stopRequested_.load(std::memory_order_acquire)) {
            // Brief backoff before attempting clean auto-reconnect (e.g. Bluetooth headset powered back on)
            Sleep(500);
        }
    }

    if (hiResTimer) CloseHandle(hiResTimer);
    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
    if (didCom) CoUninitialize();
}

} // namespace winaudio::audio
