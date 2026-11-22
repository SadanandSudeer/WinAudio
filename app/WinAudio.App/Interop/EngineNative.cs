using System.Runtime.InteropServices;

namespace WinAudio.App.Interop;

public enum WaEqMode : int
{
    Eq10Band = 0,
    Eq31Band = 1,
    Parametric = 2
}

public enum WaFilterType : int
{
    Peaking = 0,
    LowShelf = 1,
    HighShelf = 2,
    LowPass = 3,
    HighPass = 4,
    Notch = 5
}

public enum WaSurroundMode : int
{
    Off = 0,
    Headphones = 1,
    Speakers = 2
}

public enum WaDeviceTransport : int
{
    Unknown = 0,
    WiredSpeakers = 1,
    Headphones = 2,
    Hdmi = 3,
    Usb = 4,
    BluetoothA2dp = 5,
    BluetoothHfp = 6,
    Virtual = 7
}

public enum WaRoutingMode : int
{
    Stopped = 0,
    VirtualSink = 1,
    DirectLoopback = 2
}

public enum WaOutputState : int
{
    Idle = 0,
    Running = 1,
    Reconnecting = 2,
    Error = 3
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct WaParamBand
{
    public int Enabled;
    public int FilterType;
    public float FrequencyHz;
    public float GainDb;
    public float Q;
    public int Reserved;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct WaEqParams
{
    public int Enabled;
    public int Mode;

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = EngineNative.MaxGraphicBands)]
    public float[] GraphicGainsDb;

    public int Reserved;

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = EngineNative.MaxParamBands)]
    public WaParamBand[] ParamBands;

    public static WaEqParams CreateDefault()
    {
        var eq = new WaEqParams
        {
            Enabled = 1,
            Mode = (int)WaEqMode.Eq10Band,
            GraphicGainsDb = new float[EngineNative.MaxGraphicBands],
            ParamBands = new WaParamBand[EngineNative.MaxParamBands]
        };
        float[] freqs = { 32, 64, 125, 250, 500, 1000, 2000, 4000, 6500, 8000, 12000, 16000 };
        for (int i = 0; i < EngineNative.MaxParamBands; i++)
        {
            eq.ParamBands[i] = new WaParamBand
            {
                Enabled = i < 8 ? 1 : 0,
                FilterType = i == 0 ? (int)WaFilterType.LowShelf : (i == 11 ? (int)WaFilterType.HighShelf : (int)WaFilterType.Peaking),
                FrequencyHz = freqs[i],
                GainDb = 0f,
                Q = 1.2f
            };
        }
        return eq;
    }
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct WaDspParams
{
    public int GlobalBypass;
    public float PreAmpDb;

    public int BassEnabled;
    public float BassBoostDb;
    public float BassFreqHz;
    public float BassExciterMix;

    public int TrebleEnabled;
    public float TrebleBoostDb;
    public float TrebleFreqHz;
    public float ClarityBoostDb;

    public WaEqParams Eq;

    public int LoudnessEnabled;
    public float LoudnessTargetLufs;
    public float LoudnessStrength;
    public float LoudnessMaxGainDb;

    public int WidenerEnabled;
    public float StereoWidth;
    public float BassMonoFreqHz;

    public int SurroundMode;
    public float SurroundDepth;

    public int AmbienceEnabled;
    public float AmbienceRoomSize;
    public float AmbienceDamping;
    public float AmbienceMix;

    public int MaximizerEnabled;
    public float MaximizerBoostDb;
    public float MaximizerCeilingDb;
    public float MaximizerSoftClip;
    public float MaximizerReleaseMs;
}

[StructLayout(LayoutKind.Sequential, Pack = 8, CharSet = CharSet.Unicode)]
public struct WaDeviceInfo
{
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = EngineNative.DeviceIdLen)]
    public string Id;

    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = EngineNative.DeviceNameLen)]
    public string Name;

    public int Transport;
    public int IsDefault;
    public int IsDefaultComm;
    public int IsVirtualSink;
    public int IsBluetoothHfp;
    public int SampleRate;
    public int Channels;
    public int IsActive;
}

[StructLayout(LayoutKind.Sequential, Pack = 8, CharSet = CharSet.Unicode)]
public struct WaOutputSlotConfig
{
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = EngineNative.DeviceIdLen)]
    public string DeviceId;

    public int Enabled;
    public float Volume;
    public int Mute;
    public float Balance;
    public float SyncDelayMs;
    public int UseCustomEq;
    public WaEqParams CustomEq;
}

[StructLayout(LayoutKind.Sequential, Pack = 8, CharSet = CharSet.Unicode)]
public struct WaOutputTelemetry
{
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = EngineNative.DeviceIdLen)]
    public string DeviceId;

    public int State;
    public int SampleRate;
    public float BufferFillMs;
    public float DriftPpm;
    public float AppliedDelayMs;
    public uint Underruns;
    public uint Overruns;
    public int Reserved;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct WaTelemetry
{
    public int RoutingMode;
    public int IsRunning;
    public int IsBypassed;
    public int EngineSampleRate;

    public float InputPeakDbL;
    public float InputPeakDbR;
    public float InputRmsDbL;
    public float InputRmsDbR;

    public float OutputPeakDbL;
    public float OutputPeakDbR;
    public float OutputRmsDbL;
    public float OutputRmsDbR;

    public float LimiterGrDb;
    public float LoudnessGainDb;
    public float CpuLoadPercent;

    public int ActiveOutputCount;

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = EngineNative.SpectrumBins)]
    public float[] SpectrumDb;

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = EngineNative.MaxOutputSlots)]
    public WaOutputTelemetry[] Outputs;
}

[StructLayout(LayoutKind.Sequential, Pack = 8, CharSet = CharSet.Unicode)]
public struct WaCalibrationResult
{
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = EngineNative.DeviceIdLen)]
    public string DeviceId;

    public float MeasuredLatencyMs;
    public float RecommendedDelayMs;
    public float MeasuredLevelDb;
    public float RecommendedVolume;
    public float Confidence;
    public int Succeeded;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct WaAbiSizes
{
    public int AbiVersion;
    public int SizeParamBand;
    public int SizeEqParams;
    public int SizeDspParams;
    public int SizeDeviceInfo;
    public int SizeOutputSlotConfig;
    public int SizeOutputTelemetry;
    public int SizeTelemetry;
    public int SizeCalibrationResult;
}

[UnmanagedFunctionPointer(CallingConvention.StdCall)]
public delegate void WaDeviceChangeCallback(IntPtr userContext);

public static class EngineNative
{
    public const string DllName = "WinAudioEngine.dll";
    public const int AbiVersion = 1;
    public const int MaxGraphicBands = 31;
    public const int MaxParamBands = 12;
    public const int MaxOutputSlots = 8;
    public const int MaxDevices = 64;
    public const int SpectrumBins = 64;
    public const int DeviceIdLen = 128;
    public const int DeviceNameLen = 160;

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void wa_get_abi_sizes(out WaAbiSizes outSizes);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void wa_init_default_dsp_params(out WaDspParams outParams);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr wa_create();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void wa_destroy(IntPtr handle);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern int wa_start(IntPtr handle, string? preferredVirtualSinkId, int preferVirtualMode, int bufferMs);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void wa_stop(IntPtr handle);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int wa_get_routing_mode(IntPtr handle);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void wa_set_master_dsp(IntPtr handle, ref WaDspParams paramsRef);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void wa_set_bypass(IntPtr handle, int bypass);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void wa_compute_eq_curve(ref WaDspParams paramsRef, float sampleRate, [In] float[] freqsHz, [Out] float[] outDb, int count);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int wa_enumerate_devices(IntPtr handle, [Out] WaDeviceInfo[] outDevices, int maxCount);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern int wa_set_default_device(IntPtr handle, string deviceId, int includeCommunications);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern int wa_get_endpoint_volume(IntPtr handle, string deviceId, out float outVolume01, out int outMuted, out float outBalance);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern int wa_set_endpoint_volume(IntPtr handle, string deviceId, float volume01, int muted, float balance);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int wa_configure_outputs(IntPtr handle, [In] WaOutputSlotConfig[] slots, int count);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void wa_trigger_sync_pulse(IntPtr handle);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void wa_set_test_tone(IntPtr handle, int enabled, float freqHz, float gainDb);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern int wa_get_microphone_info(IntPtr handle, System.Text.StringBuilder outMicName, int maxNameLen);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern int wa_run_mic_calibration(IntPtr handle, [In] string[] deviceIds, int deviceCount, [Out] WaCalibrationResult[] outResults);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void wa_get_telemetry(IntPtr handle, out WaTelemetry outTelemetry, int includeSpectrum);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void wa_register_device_change_callback(IntPtr handle, WaDeviceChangeCallback? callback, IntPtr userContext);
}
