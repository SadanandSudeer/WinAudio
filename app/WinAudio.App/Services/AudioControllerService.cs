using WinAudio.App.Interop;
using WinAudio.App.Models;

namespace WinAudio.App.Services;

public sealed class AudioControllerService : IDisposable
{
    private IntPtr _handle;
    private readonly WaDeviceChangeCallback _deviceChangeCallback;
    private bool _disposed;

    public event Action? DevicesChanged;

    public bool IsInitialized => _handle != IntPtr.Zero;

    public AudioControllerService()
    {
        _deviceChangeCallback = OnNativeDevicesChanged;
        _handle = EngineNative.wa_create();
        if (_handle != IntPtr.Zero)
        {
            EngineNative.wa_register_device_change_callback(_handle, _deviceChangeCallback, IntPtr.Zero);
        }
    }

    private void OnNativeDevicesChanged(IntPtr _)
    {
        DevicesChanged?.Invoke();
    }

    public bool Start(string? preferredVirtualSinkId, bool preferVirtualMode, int bufferMs)
    {
        if (_handle == IntPtr.Zero) return false;
        return EngineNative.wa_start(_handle, preferredVirtualSinkId, preferVirtualMode ? 1 : 0, bufferMs) != 0;
    }

    public void Stop()
    {
        if (_handle == IntPtr.Zero) return;
        EngineNative.wa_stop(_handle);
    }

    public WaRoutingMode GetRoutingMode()
    {
        if (_handle == IntPtr.Zero) return WaRoutingMode.Stopped;
        return (WaRoutingMode)EngineNative.wa_get_routing_mode(_handle);
    }

    public void ApplyProfile(AudioProfile profile, bool globalBypass)
    {
        if (_handle == IntPtr.Zero) return;
        var native = profile.ToNative(globalBypass);
        EngineNative.wa_set_master_dsp(_handle, ref native);
    }

    public void SetBypass(bool bypass)
    {
        if (_handle == IntPtr.Zero) return;
        EngineNative.wa_set_bypass(_handle, bypass ? 1 : 0);
    }

    public float[] ComputeEqCurve(AudioProfile profile, float sampleRate, float[] freqsHz)
    {
        var outDb = new float[freqsHz.Length];
        var native = profile.ToNative(false);
        EngineNative.wa_compute_eq_curve(ref native, sampleRate, freqsHz, outDb, freqsHz.Length);
        return outDb;
    }

    public List<WaDeviceInfo> EnumerateDevices()
    {
        if (_handle == IntPtr.Zero) return new List<WaDeviceInfo>();
        var raw = new WaDeviceInfo[EngineNative.MaxDevices];
        int count = EngineNative.wa_enumerate_devices(_handle, raw, EngineNative.MaxDevices);
        var list = new List<WaDeviceInfo>(Math.Max(0, count));
        for (int i = 0; i < count; i++) list.Add(raw[i]);
        return list;
    }

    public bool SetDefaultDevice(string deviceId, bool includeCommunications = true)
    {
        if (_handle == IntPtr.Zero || string.IsNullOrWhiteSpace(deviceId)) return false;
        return EngineNative.wa_set_default_device(_handle, deviceId, includeCommunications ? 1 : 0) != 0;
    }

    public bool TryGetEndpointVolume(string deviceId, out float volume01, out bool muted, out float balance)
    {
        volume01 = 1.0f;
        muted = false;
        balance = 0.0f;
        if (_handle == IntPtr.Zero || string.IsNullOrWhiteSpace(deviceId)) return false;
        int ok = EngineNative.wa_get_endpoint_volume(_handle, deviceId, out volume01, out int m, out balance);
        muted = m != 0;
        return ok != 0;
    }

    public bool SetEndpointVolume(string deviceId, float volume01, bool muted, float balance)
    {
        if (_handle == IntPtr.Zero || string.IsNullOrWhiteSpace(deviceId)) return false;
        return EngineNative.wa_set_endpoint_volume(_handle, deviceId, Math.Clamp(volume01, 0f, 1f), muted ? 1 : 0, Math.Clamp(balance, -1f, 1f)) != 0;
    }

    public bool ConfigureOutputs(IEnumerable<DeviceViewModel> enabledDevices, IReadOnlyList<AudioProfile> allPresets)
    {
        if (_handle == IntPtr.Zero) return false;
        var list = enabledDevices.Where(d => d.IsMultiEnabled).Take(EngineNative.MaxOutputSlots).ToList();
        var slots = new WaOutputSlotConfig[list.Count];

        for (int i = 0; i < list.Count; i++)
        {
            var d = list[i];
            AudioProfile? customProf = string.IsNullOrWhiteSpace(d.CustomEqProfileId)
                ? null
                : allPresets.FirstOrDefault(p => p.Id == d.CustomEqProfileId);

            slots[i] = new WaOutputSlotConfig
            {
                DeviceId = d.Id,
                Enabled = 1,
                Volume = d.Volume,
                Mute = d.Mute ? 1 : 0,
                Balance = d.Balance,
                SyncDelayMs = d.SyncDelayMs,
                UseCustomEq = customProf != null ? 1 : 0,
                CustomEq = customProf != null ? customProf.Eq.ToNative() : WaEqParams.CreateDefault()
            };
        }

        return EngineNative.wa_configure_outputs(_handle, slots, slots.Length) != 0;
    }

    public void TriggerSyncPulse()
    {
        if (_handle == IntPtr.Zero) return;
        EngineNative.wa_trigger_sync_pulse(_handle);
    }

    public void SetTestTone(bool enabled, float freqHz = 440f, float gainDb = -16f)
    {
        if (_handle == IntPtr.Zero) return;
        EngineNative.wa_set_test_tone(_handle, enabled ? 1 : 0, freqHz, gainDb);
    }

    public bool TryGetMicrophone(out string micName)
    {
        micName = "";
        if (_handle == IntPtr.Zero) return false;
        var sb = new System.Text.StringBuilder(EngineNative.DeviceNameLen);
        int ok = EngineNative.wa_get_microphone_info(_handle, sb, sb.Capacity);
        if (ok != 0)
        {
            micName = sb.ToString();
            return true;
        }
        return false;
    }

    public Task<WaCalibrationResult[]> RunMicCalibrationAsync(IReadOnlyList<string> deviceIds)
    {
        return Task.Run(() =>
        {
            if (_handle == IntPtr.Zero || deviceIds.Count == 0) return Array.Empty<WaCalibrationResult>();
            var ids = deviceIds.Take(EngineNative.MaxOutputSlots).ToArray();
            var results = new WaCalibrationResult[ids.Length];
            int count = EngineNative.wa_run_mic_calibration(_handle, ids, ids.Length, results);
            return count > 0 ? results[..count] : Array.Empty<WaCalibrationResult>();
        });
    }

    public WaTelemetry GetTelemetry(bool includeSpectrum)
    {
        if (_handle == IntPtr.Zero) return default;
        EngineNative.wa_get_telemetry(_handle, out var t, includeSpectrum ? 1 : 0);
        return t;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_handle != IntPtr.Zero)
        {
            EngineNative.wa_register_device_change_callback(_handle, null, IntPtr.Zero);
            EngineNative.wa_stop(_handle);
            EngineNative.wa_destroy(_handle);
            _handle = IntPtr.Zero;
        }
    }
}
