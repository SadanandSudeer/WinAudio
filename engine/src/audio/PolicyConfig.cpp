#include "PolicyConfig.h"
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <comdef.h>
#include <algorithm>

namespace {

// Undocumented but stable Windows Vista/7/10/11 IPolicyConfig COM interface definitions
interface DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

interface DECLSPEC_UUID("568b9108-44bf-40b4-9006-86afe5b5a620") IPolicyConfigVista : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

const CLSID CLSID_CPolicyConfigClient = { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };
const CLSID CLSID_CPolicyConfigVistaClient = { 0x294935ce, 0xf637, 0x4e7c, { 0xa4, 0x1b, 0xab, 0x25, 0x54, 0x60, 0xb8, 0x62 } };

} // namespace

namespace winaudio::audio {

bool setSystemDefaultEndpoint(const std::wstring& deviceId, bool includeCommunications) noexcept {
    if (deviceId.empty()) return false;

    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didInit = SUCCEEDED(initHr);

    bool ok = false;

    IPolicyConfig* pPolicy = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), reinterpret_cast<void**>(&pPolicy));
    if (SUCCEEDED(hr) && pPolicy) {
        HRESULT hr1 = pPolicy->SetDefaultEndpoint(deviceId.c_str(), eConsole);
        HRESULT hr2 = pPolicy->SetDefaultEndpoint(deviceId.c_str(), eMultimedia);
        if (includeCommunications) {
            pPolicy->SetDefaultEndpoint(deviceId.c_str(), eCommunications);
        }
        ok = SUCCEEDED(hr1) || SUCCEEDED(hr2);
        pPolicy->Release();
    } else {
        IPolicyConfigVista* pVista = nullptr;
        hr = CoCreateInstance(CLSID_CPolicyConfigVistaClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfigVista), reinterpret_cast<void**>(&pVista));
        if (SUCCEEDED(hr) && pVista) {
            HRESULT hr1 = pVista->SetDefaultEndpoint(deviceId.c_str(), eConsole);
            HRESULT hr2 = pVista->SetDefaultEndpoint(deviceId.c_str(), eMultimedia);
            if (includeCommunications) {
                pVista->SetDefaultEndpoint(deviceId.c_str(), eCommunications);
            }
            ok = SUCCEEDED(hr1) || SUCCEEDED(hr2);
            pVista->Release();
        }
    }

    if (didInit) {
        CoUninitialize();
    }
    return ok;
}

bool getEndpointVolume(const std::wstring& deviceId, float& outVolume01, bool& outMuted, float& outBalance) noexcept {
    if (deviceId.empty()) return false;
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didInit = SUCCEEDED(initHr);

    bool ok = false;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioEndpointVolume* epVol = nullptr;

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator) {
        if (SUCCEEDED(enumerator->GetDevice(deviceId.c_str(), &device)) && device) {
            if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(&epVol))) && epVol) {
                BOOL muted = FALSE;
                epVol->GetMute(&muted);
                outMuted = (muted != FALSE);

                UINT chCount = 0;
                epVol->GetChannelCount(&chCount);
                if (chCount >= 2) {
                    float chL = 1.0f, chR = 1.0f;
                    if (SUCCEEDED(epVol->GetChannelVolumeLevelScalar(0, &chL)) &&
                        SUCCEEDED(epVol->GetChannelVolumeLevelScalar(1, &chR))) {
                        float peak = std::max(chL, chR);
                        outVolume01 = std::clamp(peak, 0.0f, 1.0f);
                        if (peak > 0.001f) {
                            outBalance = (chR >= chL)
                                ? std::clamp(1.0f - (chL / chR), 0.0f, 1.0f)
                                : -std::clamp(1.0f - (chR / chL), 0.0f, 1.0f);
                        } else {
                            outBalance = 0.0f;
                        }
                        ok = true;
                    }
                }
                if (!ok) {
                    float master = 1.0f;
                    if (SUCCEEDED(epVol->GetMasterVolumeLevelScalar(&master))) {
                        outVolume01 = std::clamp(master, 0.0f, 1.0f);
                        outBalance = 0.0f;
                        ok = true;
                    }
                }
                epVol->Release();
            }
            device->Release();
        }
        enumerator->Release();
    }

    if (didInit) CoUninitialize();
    return ok;
}

bool setEndpointVolume(const std::wstring& deviceId, float volume01, bool muted, float balance) noexcept {
    if (deviceId.empty()) return false;
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didInit = SUCCEEDED(initHr);

    bool ok = false;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioEndpointVolume* epVol = nullptr;

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator) {
        if (SUCCEEDED(enumerator->GetDevice(deviceId.c_str(), &device)) && device) {
            if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(&epVol))) && epVol) {
                HRESULT hrMute = epVol->SetMute(muted ? TRUE : FALSE, nullptr);
                const float vol = std::clamp(volume01, 0.0f, 1.0f);
                const float bal = std::clamp(balance, -1.0f, 1.0f);

                UINT chCount = 0;
                epVol->GetChannelCount(&chCount);
                HRESULT hrVol = E_FAIL;
                if (chCount >= 2) {
                    const float chL = std::clamp(vol * std::min(1.0f, 1.0f - bal), 0.0f, 1.0f);
                    const float chR = std::clamp(vol * std::min(1.0f, 1.0f + bal), 0.0f, 1.0f);
                    HRESULT h1 = epVol->SetChannelVolumeLevelScalar(0, chL, nullptr);
                    HRESULT h2 = epVol->SetChannelVolumeLevelScalar(1, chR, nullptr);
                    hrVol = (SUCCEEDED(h1) || SUCCEEDED(h2)) ? S_OK : E_FAIL;
                }
                if (FAILED(hrVol)) {
                    hrVol = epVol->SetMasterVolumeLevelScalar(vol, nullptr);
                }
                ok = SUCCEEDED(hrMute) || SUCCEEDED(hrVol);
                epVol->Release();
            }
            device->Release();
        }
        enumerator->Release();
    }

    if (didInit) CoUninitialize();
    return ok;
}

} // namespace winaudio::audio
