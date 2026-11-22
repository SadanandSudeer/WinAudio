#include "WinAudioABI.h"
#include "audio/AudioEngine.h"
#include "audio/PolicyConfig.h"
#include "audio/AcousticCalibrator.h"
#include "dsp/MasterDspChain.h"
#include <algorithm>
#include <cstring>

using winaudio::audio::AudioEngine;
using winaudio::dsp::MasterDspChain;

extern "C" {

WA_API void wa_get_abi_sizes(WaAbiSizes* outSizes) {
    if (!outSizes) return;
    outSizes->abiVersion = WA_ABI_VERSION;
    outSizes->sizeParamBand = static_cast<int32_t>(sizeof(WaParamBand));
    outSizes->sizeEqParams = static_cast<int32_t>(sizeof(WaEqParams));
    outSizes->sizeDspParams = static_cast<int32_t>(sizeof(WaDspParams));
    outSizes->sizeDeviceInfo = static_cast<int32_t>(sizeof(WaDeviceInfo));
    outSizes->sizeOutputSlotConfig = static_cast<int32_t>(sizeof(WaOutputSlotConfig));
    outSizes->sizeOutputTelemetry = static_cast<int32_t>(sizeof(WaOutputTelemetry));
    outSizes->sizeTelemetry = static_cast<int32_t>(sizeof(WaTelemetry));
    outSizes->sizeCalibrationResult = static_cast<int32_t>(sizeof(WaCalibrationResult));
}

WA_API void wa_init_default_dsp_params(WaDspParams* outParams) {
    if (!outParams) return;
    *outParams = MasterDspChain::defaultParams();
}

WA_API WaEngineHandle wa_create(void) {
    return static_cast<WaEngineHandle>(new AudioEngine());
}

WA_API void wa_destroy(WaEngineHandle handle) {
    if (!handle) return;
    delete static_cast<AudioEngine*>(handle);
}

WA_API int32_t wa_start(WaEngineHandle handle, const wchar_t* preferredVirtualSinkId, int32_t preferVirtualMode, int32_t bufferMs) {
    if (!handle) return 0;
    auto* eng = static_cast<AudioEngine*>(handle);
    std::wstring pref = (preferredVirtualSinkId && preferredVirtualSinkId[0]) ? std::wstring(preferredVirtualSinkId) : L"";
    return eng->start(pref, preferVirtualMode != 0, bufferMs) ? 1 : 0;
}

WA_API void wa_stop(WaEngineHandle handle) {
    if (!handle) return;
    static_cast<AudioEngine*>(handle)->stop();
}

WA_API int32_t wa_get_routing_mode(WaEngineHandle handle) {
    if (!handle) return WA_ROUTING_STOPPED;
    return static_cast<int32_t>(static_cast<AudioEngine*>(handle)->routingMode());
}

WA_API void wa_set_master_dsp(WaEngineHandle handle, const WaDspParams* params) {
    if (!handle || !params) return;
    static_cast<AudioEngine*>(handle)->setMasterDsp(*params);
}

WA_API void wa_set_bypass(WaEngineHandle handle, int32_t bypass) {
    if (!handle) return;
    static_cast<AudioEngine*>(handle)->setBypass(bypass != 0);
}

WA_API void wa_compute_eq_curve(const WaDspParams* params, float sampleRate, const float* freqsHz, float* outDb, int32_t count) {
    if (!params || !freqsHz || !outDb || count <= 0) return;
    MasterDspChain::computeAnalyticalCurve(*params, sampleRate, freqsHz, outDb, count);
}

WA_API int32_t wa_enumerate_devices(WaEngineHandle handle, WaDeviceInfo* outDevices, int32_t maxCount) {
    if (!handle || !outDevices || maxCount <= 0) return 0;
    auto list = static_cast<AudioEngine*>(handle)->enumerateDevices();
    int32_t n = std::min(static_cast<int32_t>(list.size()), maxCount);
    for (int32_t i = 0; i < n; ++i) {
        outDevices[i] = list[i];
    }
    return n;
}

WA_API int32_t wa_set_default_device(WaEngineHandle handle, const wchar_t* deviceId, int32_t includeCommunications) {
    if (!handle || !deviceId || !deviceId[0]) return 0;
    return static_cast<AudioEngine*>(handle)->setDefaultDevice(deviceId, includeCommunications != 0) ? 1 : 0;
}

WA_API int32_t wa_get_endpoint_volume(WaEngineHandle, const wchar_t* deviceId, float* outVolume01, int32_t* outMuted, float* outBalance) {
    if (!deviceId || !deviceId[0]) return 0;
    float vol = 1.0f, bal = 0.0f;
    bool muted = false;
    if (!winaudio::audio::getEndpointVolume(deviceId, vol, muted, bal)) return 0;
    if (outVolume01) *outVolume01 = vol;
    if (outMuted) *outMuted = muted ? 1 : 0;
    if (outBalance) *outBalance = bal;
    return 1;
}

WA_API int32_t wa_set_endpoint_volume(WaEngineHandle, const wchar_t* deviceId, float volume01, int32_t muted, float balance) {
    if (!deviceId || !deviceId[0]) return 0;
    return winaudio::audio::setEndpointVolume(deviceId, volume01, muted != 0, balance) ? 1 : 0;
}

WA_API int32_t wa_configure_outputs(WaEngineHandle handle, const WaOutputSlotConfig* slots, int32_t count) {
    if (!handle) return 0;
    return static_cast<AudioEngine*>(handle)->configureOutputs(slots, count) ? 1 : 0;
}

WA_API void wa_trigger_sync_pulse(WaEngineHandle handle) {
    if (!handle) return;
    static_cast<AudioEngine*>(handle)->triggerSyncPulse();
}

WA_API void wa_set_test_tone(WaEngineHandle handle, int32_t enabled, float freqHz, float gainDb) {
    if (!handle) return;
    static_cast<AudioEngine*>(handle)->setTestTone(enabled != 0, freqHz, gainDb);
}

WA_API int32_t wa_get_microphone_info(WaEngineHandle, wchar_t* outMicName, int32_t maxNameLen) {
    std::wstring micId, micName;
    if (!winaudio::audio::AcousticCalibrator::findActiveMicrophone(micId, micName)) {
        if (outMicName && maxNameLen > 0) outMicName[0] = L'\0';
        return 0;
    }
    if (outMicName && maxNameLen > 0) {
        size_t n = std::min(static_cast<size_t>(maxNameLen - 1), micName.size());
        std::wmemcpy(outMicName, micName.c_str(), n);
        outMicName[n] = L'\0';
    }
    return 1;
}

WA_API int32_t wa_run_mic_calibration(WaEngineHandle handle, const wchar_t* const* deviceIds, int32_t deviceCount, WaCalibrationResult* outResults) {
    if (!handle || !deviceIds || !outResults || deviceCount <= 0) return 0;
    int32_t count = std::min(deviceCount, WA_MAX_OUTPUT_SLOTS);
    std::vector<std::wstring> ids;
    ids.reserve(count);
    for (int32_t i = 0; i < count; ++i) {
        if (deviceIds[i] && deviceIds[i][0]) ids.emplace_back(deviceIds[i]);
    }
    std::vector<WaCalibrationResult> res;
    int32_t n = static_cast<AudioEngine*>(handle)->runMicCalibration(ids, res);
    for (int32_t i = 0; i < n && i < count; ++i) {
        outResults[i] = res[i];
    }
    return n;
}

WA_API void wa_get_telemetry(WaEngineHandle handle, WaTelemetry* outTelemetry, int32_t includeSpectrum) {
    if (!handle || !outTelemetry) return;
    static_cast<AudioEngine*>(handle)->getTelemetry(outTelemetry, includeSpectrum != 0);
}

WA_API void wa_register_device_change_callback(WaEngineHandle handle, WaDeviceChangeCallback callback, void* userContext) {
    if (!handle) return;
    static_cast<AudioEngine*>(handle)->registerDeviceChangeCallback(callback, userContext);
}

} // extern "C"
