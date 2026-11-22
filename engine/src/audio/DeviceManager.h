#pragma once

#include "WinAudioABI.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace winaudio::audio {

// Enumerates active Windows WASAPI render endpoints, classifies transport type
// (Wired, Headphones, HDMI, USB, Bluetooth A2DP/HFP, Virtual Sink),
// and monitors hot-plug / default-device events via IMMNotificationClient.
class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    bool initialize();
    void shutdown();

    std::vector<WaDeviceInfo> enumerateRenderDevices();
    std::wstring getDefaultRenderDeviceId(EDataFlow flow = eRender, ERole role = eMultimedia);

    // Finds best virtual sink render device (prefers VB-CABLE Input, or user-selected ID)
    std::wstring findVirtualSinkId(const std::wstring& preferredId = L"");

    // Finds paired capture endpoint for VB-CABLE (e.g. "CABLE Output") if present
    std::wstring findPairedVirtualCaptureId(const std::wstring& virtualRenderId);

    void setChangeCallback(std::function<void()> cb);

private:
    class NotificationClient;

    static WaDeviceTransport classifyTransport(const std::wstring& friendlyName,
                                               const std::wstring& ifaceName,
                                               uint32_t formFactor,
                                               bool& outIsVirtual,
                                               bool& outIsBtHfp);

    IMMDeviceEnumerator* enumerator_ = nullptr;
    NotificationClient* notifClient_ = nullptr;
    bool comInitialized_ = false;
    std::mutex cbMutex_;
    std::function<void()> onChanged_;
};

} // namespace winaudio::audio
