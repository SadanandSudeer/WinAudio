#include "DeviceManager.h"
#include <initguid.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <atomic>
#include <cwctype>

// Define PKEY_AudioEndpoint_FormFactor locally (GUID {1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E}, pid 0)
static const PROPERTYKEY kPkeyAudioEndpointFormFactor = {
    { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0
};
// DEVPKEY_Device_EnumeratorName ({A45C254E-DF1C-4EFD-8020-67D146A850E0}, pid 24)
static const PROPERTYKEY kPkeyDeviceEnumeratorName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 24
};
// PKEY_Device_DeviceDesc ({A45C254E-DF1C-4EFD-8020-67D146A850E0}, pid 2)
static const PROPERTYKEY kPkeyDeviceDesc = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2
};
// PKEY_Device_BusReportedDeviceDesc / InstanceId ({78C34FC8-104A-4ACA-9EA4-524D52996E57}, pid 256)
static const PROPERTYKEY kPkeyDeviceInstanceId = {
    { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57 } }, 256
};

namespace winaudio::audio {

namespace {

std::wstring toLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool containsCI(const std::wstring& haystack, const wchar_t* needle) {
    return toLower(haystack).find(toLower(needle)) != std::wstring::npos;
}

std::wstring readPropString(IPropertyStore* store, const PROPERTYKEY& key) {
    if (!store) return L"";
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring result;
    if (SUCCEEDED(store->GetValue(key, &pv))) {
        if (pv.vt == VT_LPWSTR && pv.pwszVal) {
            result = pv.pwszVal;
        }
        PropVariantClear(&pv);
    }
    return result;
}

uint32_t readPropUint32(IPropertyStore* store, const PROPERTYKEY& key, uint32_t fallback = 0) {
    if (!store) return fallback;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    uint32_t result = fallback;
    if (SUCCEEDED(store->GetValue(key, &pv))) {
        if (pv.vt == VT_UI4) result = pv.ulVal;
        PropVariantClear(&pv);
    }
    return result;
}

void copyWideTruncated(wchar_t* dst, size_t dstLen, const std::wstring& src) {
    if (!dst || dstLen == 0) return;
    size_t n = std::min(dstLen - 1, src.size());
    std::wmemcpy(dst, src.c_str(), n);
    dst[n] = L'\0';
}

} // namespace

class DeviceManager::NotificationClient final : public IMMNotificationClient {
public:
    explicit NotificationClient(DeviceManager* owner) : owner_(owner) {}
    virtual ~NotificationClient() = default;

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refCount_;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppvInterface) override {
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
            *ppvInterface = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppvInterface = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && (role == eMultimedia || role == eConsole)) notify();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    void notify() {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(owner_->cbMutex_);
            cb = owner_->onChanged_;
        }
        if (cb) cb();
    }

    DeviceManager* owner_;
    std::atomic<ULONG> refCount_{ 1 };
};

DeviceManager::DeviceManager() = default;

DeviceManager::~DeviceManager() {
    shutdown();
}

bool DeviceManager::initialize() {
    if (enumerator_) return true;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        comInitialized_ = true;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr) || !enumerator_) {
        enumerator_ = nullptr;
        return false;
    }

    notifClient_ = new NotificationClient(this);
    enumerator_->RegisterEndpointNotificationCallback(notifClient_);
    return true;
}

void DeviceManager::shutdown() {
    if (enumerator_) {
        if (notifClient_) {
            enumerator_->UnregisterEndpointNotificationCallback(notifClient_);
            notifClient_->Release();
            notifClient_ = nullptr;
        }
        enumerator_->Release();
        enumerator_ = nullptr;
    }
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

void DeviceManager::setChangeCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    onChanged_ = std::move(cb);
}

std::wstring DeviceManager::getDefaultRenderDeviceId(EDataFlow flow, ERole role) {
    if (!enumerator_) return L"";
    IMMDevice* dev = nullptr;
    if (FAILED(enumerator_->GetDefaultAudioEndpoint(flow, role, &dev)) || !dev) {
        return L"";
    }
    LPWSTR rawId = nullptr;
    std::wstring id;
    if (SUCCEEDED(dev->GetId(&rawId)) && rawId) {
        id = rawId;
        CoTaskMemFree(rawId);
    }
    dev->Release();
    return id;
}

WaDeviceTransport DeviceManager::classifyTransport(const std::wstring& friendlyName,
                                                   const std::wstring& ifaceName,
                                                   uint32_t formFactor,
                                                   bool& outIsVirtual,
                                                   bool& outIsBtHfp) {
    outIsVirtual = false;
    outIsBtHfp = false;

    const std::wstring combined = friendlyName + L" " + ifaceName;

    // Virtual audio cables / sinks
    if (containsCI(combined, L"vb-audio") ||
        containsCI(combined, L"cable input") ||
        containsCI(combined, L"voicemeeter") ||
        containsCI(combined, L"virtual audio") ||
        containsCI(combined, L"virtual cable") ||
        containsCI(combined, L"vac ") ||
        containsCI(combined, L"winaudio virtual")) {
        outIsVirtual = true;
        return WA_TRANSPORT_VIRTUAL;
    }

    // Bluetooth detection via Windows PnP enumerator/instance ID + friendly name (distinguish HFP vs A2DP!)
    if (containsCI(combined, L"hands-free") || containsCI(combined, L"ag audio") ||
        containsCI(combined, L"hfp") || containsCI(combined, L"bthhfenum")) {
        outIsBtHfp = true;
        return WA_TRANSPORT_BLUETOOTH_HFP;
    }
    if (containsCI(combined, L"bthenum") ||
        containsCI(combined, L"btha2dp") ||
        containsCI(combined, L"bluetooth") ||
        containsCI(combined, L"a2dp") ||
        containsCI(combined, L"airpods") ||
        containsCI(combined, L"jbl") ||
        containsCI(combined, L"wh-1000") ||
        containsCI(combined, L"buds")) {
        return WA_TRANSPORT_BLUETOOTH_A2DP;
    }

    // HDMI / DisplayPort / Monitor audio
    if (formFactor == HDMI || formFactor == DigitalAudioDisplayDevice ||
        containsCI(combined, L"hdmi") || containsCI(combined, L"displayport") ||
        containsCI(combined, L"nvidia high definition") || containsCI(combined, L"amd high definition")) {
        return WA_TRANSPORT_HDMI;
    }

    // USB Audio
    if (containsCI(combined, L"usb")) {
        return WA_TRANSPORT_USB;
    }

    // Headphones / Headsets (3.5mm or generic)
    if (formFactor == Headphones || formFactor == Headset ||
        containsCI(combined, L"headphone") || containsCI(combined, L"headset") || containsCI(combined, L"earphone")) {
        return WA_TRANSPORT_HEADPHONES;
    }

    return WA_TRANSPORT_WIRED_SPEAKERS;
}

std::vector<WaDeviceInfo> DeviceManager::enumerateRenderDevices() {
    std::vector<WaDeviceInfo> list;
    if (!enumerator_) return list;

    const std::wstring defMult = getDefaultRenderDeviceId(eRender, eMultimedia);
    const std::wstring defComm = getDefaultRenderDeviceId(eRender, eCommunications);

    IMMDeviceCollection* col = nullptr;
    if (FAILED(enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) || !col) {
        return list;
    }

    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count && list.size() < WA_MAX_DEVICES; ++i) {
        IMMDevice* dev = nullptr;
        if (FAILED(col->Item(i, &dev)) || !dev) continue;

        WaDeviceInfo info{};
        info.isActive = 1;

        LPWSTR rawId = nullptr;
        std::wstring id;
        if (SUCCEEDED(dev->GetId(&rawId)) && rawId) {
            id = rawId;
            CoTaskMemFree(rawId);
        }

        IPropertyStore* store = nullptr;
        std::wstring friendly, iface, enumName, instId, devDesc;
        uint32_t formFactor = Speakers;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)) && store) {
            friendly = readPropString(store, PKEY_Device_FriendlyName);
            iface = readPropString(store, PKEY_DeviceInterface_FriendlyName);
            enumName = readPropString(store, kPkeyDeviceEnumeratorName);
            instId = readPropString(store, kPkeyDeviceInstanceId);
            devDesc = readPropString(store, kPkeyDeviceDesc);
            formFactor = readPropUint32(store, kPkeyAudioEndpointFormFactor, Speakers);
            store->Release();
        }

        // Query native shared-mode mix format (sample rate & channels)
        IAudioClient* client = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) && client) {
            WAVEFORMATEX* mixFmt = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mixFmt)) && mixFmt) {
                info.sampleRate = static_cast<int32_t>(mixFmt->nSamplesPerSec);
                info.channels = static_cast<int32_t>(mixFmt->nChannels);
                CoTaskMemFree(mixFmt);
            }
            client->Release();
        }

        bool isVirtual = false;
        bool isBtHfp = false;
        const std::wstring meta = iface + L" " + enumName + L" " + instId + L" " + devDesc;
        info.transport = static_cast<int32_t>(classifyTransport(friendly, meta, formFactor, isVirtual, isBtHfp));
        info.isVirtualSink = isVirtual ? 1 : 0;
        info.isBluetoothHfp = isBtHfp ? 1 : 0;
        info.isDefault = (!id.empty() && id == defMult) ? 1 : 0;
        info.isDefaultComm = (!id.empty() && id == defComm) ? 1 : 0;

        copyWideTruncated(info.id, WA_DEVICE_ID_LEN, id);
        copyWideTruncated(info.name, WA_DEVICE_NAME_LEN, friendly.empty() ? id : friendly);

        list.push_back(info);
        dev->Release();
    }

    col->Release();
    return list;
}

std::wstring DeviceManager::findVirtualSinkId(const std::wstring& preferredId) {
    auto devices = enumerateRenderDevices();
    if (!preferredId.empty()) {
        for (const auto& d : devices) {
            if (preferredId == d.id) return preferredId;
        }
    }
    // Prefer VB-Audio CABLE Input first
    for (const auto& d : devices) {
        if (containsCI(d.name, L"cable input") && containsCI(d.name, L"vb-audio")) {
            return d.id;
        }
    }
    // Any VB-Audio / virtual cable sink (avoid hijacking third-party apps like Boom unless user chose it)
    for (const auto& d : devices) {
        if (d.isVirtualSink && (containsCI(d.name, L"vb-audio") || containsCI(d.name, L"virtual cable") || containsCI(d.name, L"voicemeeter"))) {
            return d.id;
        }
    }
    return L"";
}

std::wstring DeviceManager::findPairedVirtualCaptureId(const std::wstring& virtualRenderId) {
    if (!enumerator_ || virtualRenderId.empty()) return L"";

    // Check friendly name of render device first
    IMMDevice* renderDev = nullptr;
    std::wstring renderName;
    if (SUCCEEDED(enumerator_->GetDevice(virtualRenderId.c_str(), &renderDev)) && renderDev) {
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(renderDev->OpenPropertyStore(STGM_READ, &store)) && store) {
            renderName = readPropString(store, PKEY_Device_FriendlyName);
            store->Release();
        }
        renderDev->Release();
    }

    // Only look for paired CABLE Output if render sink is VB-CABLE Input
    if (!containsCI(renderName, L"cable input")) return L"";

    IMMDeviceCollection* col = nullptr;
    if (FAILED(enumerator_->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)) || !col) {
        return L"";
    }

    std::wstring matchId;
    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr;
        if (FAILED(col->Item(i, &dev)) || !dev) continue;

        IPropertyStore* store = nullptr;
        std::wstring capName;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)) && store) {
            capName = readPropString(store, PKEY_Device_FriendlyName);
            store->Release();
        }

        if (containsCI(capName, L"cable output") && containsCI(capName, L"vb-audio")) {
            LPWSTR rawId = nullptr;
            if (SUCCEEDED(dev->GetId(&rawId)) && rawId) {
                matchId = rawId;
                CoTaskMemFree(rawId);
            }
            dev->Release();
            break;
        }
        dev->Release();
    }
    col->Release();
    return matchId;
}

} // namespace winaudio::audio
