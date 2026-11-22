#include "AcousticCalibrator.h"
#include "dsp/DspMath.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <functional>

namespace winaudio::audio {

namespace {

bool containsCI(std::wstring hay, const wchar_t* needle) {
    std::transform(hay.begin(), hay.end(), hay.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::wstring n(needle);
    std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return hay.find(n) != std::wstring::npos;
}

bool isVirtualCaptureName(const std::wstring& name) {
    return containsCI(name, L"vb-audio") ||
           containsCI(name, L"cable output") ||
           containsCI(name, L"voicemeeter") ||
           containsCI(name, L"virtual audio") ||
           containsCI(name, L"stereo mix");
}

bool isBluetoothMicName(const std::wstring& name) {
    return containsCI(name, L"hands-free") ||
           containsCI(name, L"headset (") ||
           containsCI(name, L"bthenum") ||
           containsCI(name, L"bluetooth");
}

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

std::vector<float> AcousticCalibrator::generateLogChirp(double sampleRate, float amplitude) {
    const double sr = std::max(8000.0, sampleRate);
    const int n = std::max(256, static_cast<int>(std::round(sr * kChirpDurationSec)));
    std::vector<float> chirp(n, 0.0f);

    const double f0 = kChirpStartHz;
    const double f1 = std::min(kChirpEndHz, sr * 0.42);
    const double T = static_cast<double>(n) / sr;
    const double k = std::log(f1 / f0);
    const int fadeSamples = std::max(16, n / 15);

    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sr;
        const double phase = dsp::kTwoPi * f0 * T / k * (std::exp(k * t / T) - 1.0);
        double env = 1.0;
        if (i < fadeSamples) {
            env = 0.5 * (1.0 - std::cos(dsp::kPi * i / fadeSamples));
        } else if (i > n - fadeSamples) {
            env = 0.5 * (1.0 - std::cos(dsp::kPi * (n - 1 - i) / fadeSamples));
        }
        chirp[i] = static_cast<float>(std::sin(phase) * env * amplitude);
    }
    return chirp;
}

ChirpAnalysis AcousticCalibrator::analyzeRecording(const std::vector<float>& micMono,
                                                   double micSampleRate,
                                                   double chirpStartOffsetSec,
                                                   double maxExpectedLatencyMs) {
    ChirpAnalysis res{};
    if (micMono.empty() || micSampleRate < 8000.0) return res;

    // Downsample by integer factor to ~12 kHz for ultra-fast, high-precision matched-filter correlation
    // (12 kHz gives 0.083 ms sample resolution, and parabolic peak interpolation yields < 0.05 ms!)
    const int decimate = std::max(1, static_cast<int>(std::round(micSampleRate / 12000.0)));
    const double workRate = micSampleRate / static_cast<double>(decimate);

    // Simple 1-pole DC blocker + anti-alias boxcar decimation into workBuf
    const int workLen = static_cast<int>(micMono.size()) / decimate;
    std::vector<float> workBuf(workLen, 0.0f);
    double dcState = 0.0;
    const double dcAlpha = 0.995;
    for (int i = 0; i < workLen; ++i) {
        double sum = 0.0;
        for (int k = 0; k < decimate; ++k) {
            double x = micMono[static_cast<size_t>(i * decimate + k)];
            dcState = dcAlpha * dcState + (1.0 - dcAlpha) * x;
            sum += (x - dcState);
        }
        workBuf[i] = static_cast<float>(sum / decimate);
    }

    auto refChirp = generateLogChirp(workRate, 1.0f);
    const int refLen = static_cast<int>(refChirp.size());
    double refEnergy = 0.0;
    for (float v : refChirp) refEnergy += static_cast<double>(v) * v;
    const double refNorm = std::sqrt(std::max(1e-12, refEnergy));

    const int startLag = std::max(0, static_cast<int>(std::floor(chirpStartOffsetSec * workRate)));
    const int maxLagOffset = static_cast<int>(std::ceil((maxExpectedLatencyMs * 0.001) * workRate));
    const int endLag = std::min(workLen - refLen - 1, startLag + maxLagOffset);

    if (endLag <= startLag + 8) return res;

    const int numLags = endLag - startLag + 1;
    std::vector<double> corrEnv(numLags, 0.0);

    double bestScore = 0.0;
    int bestIdx = 0;

    // Normalized matched-filter cross-correlation across [startLag .. endLag]
    for (int idx = 0; idx < numLags; ++idx) {
        const int lag = startLag + idx;
        double dot = 0.0;
        double segEnergy = 0.0;
        for (int j = 0; j < refLen; ++j) {
            const double s = workBuf[static_cast<size_t>(lag + j)];
            dot += s * refChirp[j];
            segEnergy += s * s;
        }
        const double score = std::abs(dot) / (refNorm * std::sqrt(std::max(1e-10, segEnergy)));
        corrEnv[idx] = score;
        if (score > bestScore) {
            bestScore = score;
            bestIdx = idx;
        }
    }

    // Compute median background correlation score for robust SNR / confidence estimation
    std::vector<double> sortedScores = corrEnv;
    std::nth_element(sortedScores.begin(), sortedScores.begin() + sortedScores.size() / 2, sortedScores.end());
    const double medianScore = std::max(1e-4, sortedScores[sortedScores.size() / 2]);
    const double peakToMedian = bestScore / medianScore;

    // Parabolic sub-sample peak interpolation for < 0.1 ms precision
    double fracOffset = 0.0;
    if (bestIdx > 0 && bestIdx < numLags - 1) {
        const double y0 = corrEnv[bestIdx - 1];
        const double y1 = corrEnv[bestIdx];
        const double y2 = corrEnv[bestIdx + 1];
        const double denom = (y0 - 2.0 * y1 + y2);
        if (std::abs(denom) > 1e-12) {
            fracOffset = std::clamp(0.5 * (y0 - y2) / denom, -0.5, 0.5);
        }
    }

    const double exactLagSamples = static_cast<double>(bestIdx) + fracOffset;
    res.latencyMs = static_cast<float>(std::max(0.0, (exactLagSamples / workRate) * 1000.0));

    // Measure acoustic RMS loudness over the detected chirp window in original micMono
    const int origStart = std::clamp((startLag + bestIdx) * decimate, 0, static_cast<int>(micMono.size()) - 1);
    const int origLen = std::min(static_cast<int>(micMono.size()) - origStart, refLen * decimate);
    double sumSq = 0.0;
    for (int i = 0; i < origLen; ++i) {
        const double v = micMono[static_cast<size_t>(origStart + i)];
        sumSq += v * v;
    }
    const float rms = static_cast<float>(std::sqrt(sumSq / std::max(1, origLen)));
    res.rmsDb = dsp::linearToDb(rms);

    // Confidence combines absolute normalized correlation + peak-to-median sharpness
    const double snrFactor = std::clamp((peakToMedian - 2.2) / 6.0, 0.0, 1.0);
    const double corrFactor = std::clamp((bestScore - 0.10) / 0.45, 0.0, 1.0);
    res.confidence = static_cast<float>(std::clamp(0.5 * snrFactor + 0.5 * corrFactor, 0.0, 1.0));
    res.detected = (bestScore >= 0.14 && peakToMedian >= 2.6 && res.rmsDb > -65.0f);
    return res;
}

void AcousticCalibrator::computeRelativeAdjustments(std::vector<WaCalibrationResult>& results) {
    float maxLatencyMs = 0.0f;
    float sumLevelDb = 0.0f;
    int validCount = 0;

    for (const auto& r : results) {
        if (r.succeeded) {
            maxLatencyMs = std::max(maxLatencyMs, r.measuredLatencyMs);
            sumLevelDb += r.measuredLevelDb;
            ++validCount;
        }
    }

    if (validCount == 0) return;
    const float avgLevelDb = sumLevelDb / static_cast<float>(validCount);

    for (auto& r : results) {
        if (!r.succeeded) {
            r.recommendedDelayMs = 0.0f;
            r.recommendedVolume = 1.0f;
            continue;
        }
        // Delay every faster device so all outputs arrive at the listener's ears at T = maxLatencyMs!
        r.recommendedDelayMs = std::clamp(std::round(maxLatencyMs - r.measuredLatencyMs), 0.0f, 500.0f);

        // Gentle loudness matching trim (clamped to +/- 6 dB, volume 0.35 .. 1.35)
        const float deltaDb = std::clamp(avgLevelDb - r.measuredLevelDb, -6.0f, 4.0f);
        r.recommendedVolume = std::clamp(dsp::dbToLinear(deltaDb), 0.35f, 1.35f);
    }
}

bool AcousticCalibrator::findActiveMicrophone(std::wstring& outMicId, std::wstring& outMicName) {
    outMicId.clear();
    outMicName.clear();

    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didCom = SUCCEEDED(initHr);

    IMMDeviceEnumerator* enumerator = nullptr;
    bool found = false;

    auto readFriendly = [](IMMDevice* dev) -> std::wstring {
        IPropertyStore* store = nullptr;
        std::wstring name;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)) && store) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv))) {
                if (pv.vt == VT_LPWSTR && pv.pwszVal) name = pv.pwszVal;
                PropVariantClear(&pv);
            }
            store->Release();
        }
        return name;
    };

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator) {
        // 1. Prefer Windows default physical NON-Bluetooth microphone first (so we never trigger BT HFP mode!)
        std::wstring btFallbackId, btFallbackName;
        IMMDevice* defMic = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &defMic)) && defMic) {
            DWORD state = 0;
            if (SUCCEEDED(defMic->GetState(&state)) && state == DEVICE_STATE_ACTIVE) {
                std::wstring name = readFriendly(defMic);
                if (!isVirtualCaptureName(name)) {
                    LPWSTR rawId = nullptr;
                    if (SUCCEEDED(defMic->GetId(&rawId)) && rawId) {
                        if (!isBluetoothMicName(name)) {
                            outMicId = rawId;
                            outMicName = name.empty() ? rawId : name;
                            found = true;
                        } else {
                            btFallbackId = rawId;
                            btFallbackName = name.empty() ? rawId : name;
                        }
                        CoTaskMemFree(rawId);
                    }
                }
            }
            defMic->Release();
        }

        // 2. Scan all active capture endpoints preferring wired/USB/internal room mics over Bluetooth HFP
        if (!found) {
            IMMDeviceCollection* col = nullptr;
            if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)) && col) {
                UINT count = 0;
                col->GetCount(&count);
                for (UINT i = 0; i < count; ++i) {
                    IMMDevice* dev = nullptr;
                    if (FAILED(col->Item(i, &dev)) || !dev) continue;
                    std::wstring name = readFriendly(dev);
                    if (!isVirtualCaptureName(name)) {
                        LPWSTR rawId = nullptr;
                        if (SUCCEEDED(dev->GetId(&rawId)) && rawId) {
                            if (!isBluetoothMicName(name)) {
                                outMicId = rawId;
                                outMicName = name.empty() ? rawId : name;
                                CoTaskMemFree(rawId);
                                found = true;
                                dev->Release();
                                break;
                            } else if (btFallbackId.empty()) {
                                btFallbackId = rawId;
                                btFallbackName = name.empty() ? rawId : name;
                            }
                            CoTaskMemFree(rawId);
                        }
                    }
                    dev->Release();
                }
                col->Release();
            }
        }

        // 3. Only use Bluetooth mic if no wired/USB/internal mic is attached
        if (!found && !btFallbackId.empty()) {
            outMicId = btFallbackId;
            outMicName = btFallbackName;
            found = true;
        }
        enumerator->Release();
    }

    if (didCom) CoUninitialize();
    return found;
}

bool AcousticCalibrator::captureFromMicrophone(const std::wstring& micId,
                                               int totalCaptureMs,
                                               int preRollMs,
                                               std::vector<float>& outMono,
                                               double& outSampleRate,
                                               double& outTriggerOffsetSec,
                                               const std::function<void()>& triggerChirp) {
    outMono.clear();
    outSampleRate = 48000.0;
    outTriggerOffsetSec = preRollMs * 0.001;

    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didCom = SUCCEEDED(initHr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* micDev = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capSvc = nullptr;
    WAVEFORMATEX* mixFmt = nullptr;
    bool ok = false;

    auto cleanup = [&]() {
        if (client) { client->Stop(); client->Release(); client = nullptr; }
        if (capSvc) { capSvc->Release(); capSvc = nullptr; }
        if (mixFmt) { CoTaskMemFree(mixFmt); mixFmt = nullptr; }
        if (micDev) { micDev->Release(); micDev = nullptr; }
        if (enumerator) { enumerator->Release(); enumerator = nullptr; }
    };

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator) {
        if (SUCCEEDED(enumerator->GetDevice(micId.c_str(), &micDev)) && micDev) {
            if (SUCCEEDED(micDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) && client) {
                if (SUCCEEDED(client->GetMixFormat(&mixFmt)) && mixFmt) {
                    const bool useFloat = isFloatFmt(mixFmt);
                    const bool usePcm16 = !useFloat && isPcm16Fmt(mixFmt);
                    if ((useFloat || usePcm16) &&
                        SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000LL, 0, mixFmt, nullptr)) &&
                        SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capSvc))) && capSvc) {
                        outSampleRate = static_cast<double>(mixFmt->nSamplesPerSec);
                        const int ch = std::max(1, static_cast<int>(mixFmt->nChannels));
                        const size_t targetFrames = static_cast<size_t>((totalCaptureMs * 0.001) * outSampleRate);
                        outMono.reserve(targetFrames + 4096);

                        if (SUCCEEDED(client->Start())) {
                            bool triggered = false;
                            const auto tStart = std::chrono::steady_clock::now();
                            const size_t preRollFrames = static_cast<size_t>((preRollMs * 0.001) * outSampleRate);

                            while (outMono.size() < targetFrames) {
                                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - tStart).count();
                                if (elapsedMs > totalCaptureMs + 600) break;

                                if (!triggered && outMono.size() >= preRollFrames) {
                                    outTriggerOffsetSec = static_cast<double>(outMono.size()) / outSampleRate;
                                    if (triggerChirp) triggerChirp();
                                    triggered = true;
                                }

                                Sleep(4);
                                UINT32 pkt = 0;
                                if (FAILED(capSvc->GetNextPacketSize(&pkt))) break;
                                while (pkt > 0) {
                                    BYTE* data = nullptr;
                                    UINT32 frames = 0;
                                    DWORD flags = 0;
                                    if (FAILED(capSvc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

                                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                                        outMono.insert(outMono.end(), frames, 0.0f);
                                    } else if (useFloat) {
                                        const float* src = reinterpret_cast<const float*>(data);
                                        for (UINT32 i = 0; i < frames; ++i) {
                                            float s = 0.0f;
                                            for (int c = 0; c < ch; ++c) s += src[i * ch + c];
                                            outMono.push_back(s / static_cast<float>(ch));
                                        }
                                    } else if (usePcm16) {
                                        const int16_t* src = reinterpret_cast<const int16_t*>(data);
                                        constexpr float inv32768 = 1.0f / 32768.0f;
                                        for (UINT32 i = 0; i < frames; ++i) {
                                            float s = 0.0f;
                                            for (int c = 0; c < ch; ++c) s += src[i * ch + c] * inv32768;
                                            outMono.push_back(s / static_cast<float>(ch));
                                        }
                                    }
                                    capSvc->ReleaseBuffer(frames);
                                    if (FAILED(capSvc->GetNextPacketSize(&pkt))) break;
                                }
                            }
                            ok = triggered && (outMono.size() >= targetFrames / 2);
                        }
                    }
                }
            }
        }
    }

    cleanup();
    if (didCom) CoUninitialize();
    return ok;
}

} // namespace winaudio::audio
