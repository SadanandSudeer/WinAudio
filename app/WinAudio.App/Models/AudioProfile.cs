using System.Text.Json.Serialization;
using WinAudio.App.Interop;

namespace WinAudio.App.Models;

public sealed class ParametricBandModel
{
    public bool Enabled { get; set; } = true;
    public WaFilterType FilterType { get; set; } = WaFilterType.Peaking;
    public float FrequencyHz { get; set; } = 1000f;
    public float GainDb { get; set; } = 0f;
    public float Q { get; set; } = 1.2f;

    public ParametricBandModel Clone() => new()
    {
        Enabled = Enabled,
        FilterType = FilterType,
        FrequencyHz = FrequencyHz,
        GainDb = GainDb,
        Q = Q
    };
}

public sealed class EqProfileModel
{
    public bool Enabled { get; set; } = true;
    public WaEqMode Mode { get; set; } = WaEqMode.Eq10Band;
    public float[] Graphic10Db { get; set; } = new float[10];
    public float[] Graphic31Db { get; set; } = new float[31];
    public List<ParametricBandModel> ParametricBands { get; set; } = CreateDefaultParametric();

    public static List<ParametricBandModel> CreateDefaultParametric()
    {
        float[] freqs = { 32, 64, 125, 250, 500, 1000, 2000, 4000, 6500, 8000, 12000, 16000 };
        var list = new List<ParametricBandModel>(12);
        for (int i = 0; i < 12; i++)
        {
            list.Add(new ParametricBandModel
            {
                Enabled = i < 8,
                FilterType = i == 0 ? WaFilterType.LowShelf : (i == 11 ? WaFilterType.HighShelf : WaFilterType.Peaking),
                FrequencyHz = freqs[i],
                GainDb = 0f,
                Q = 1.2f
            });
        }
        return list;
    }

    public EqProfileModel Clone()
    {
        var copy = new EqProfileModel
        {
            Enabled = Enabled,
            Mode = Mode,
            Graphic10Db = (float[])Graphic10Db.Clone(),
            Graphic31Db = (float[])Graphic31Db.Clone(),
            ParametricBands = ParametricBands.Select(b => b.Clone()).ToList()
        };
        return copy;
    }

    public WaEqParams ToNative()
    {
        var native = WaEqParams.CreateDefault();
        native.Enabled = Enabled ? 1 : 0;
        native.Mode = (int)Mode;

        if (Mode == WaEqMode.Eq10Band)
        {
            for (int i = 0; i < Math.Min(10, Graphic10Db.Length); i++)
                native.GraphicGainsDb[i] = Graphic10Db[i];
        }
        else if (Mode == WaEqMode.Eq31Band)
        {
            for (int i = 0; i < Math.Min(31, Graphic31Db.Length); i++)
                native.GraphicGainsDb[i] = Graphic31Db[i];
        }

        for (int i = 0; i < Math.Min(EngineNative.MaxParamBands, ParametricBands.Count); i++)
        {
            var b = ParametricBands[i];
            native.ParamBands[i] = new WaParamBand
            {
                Enabled = b.Enabled ? 1 : 0,
                FilterType = (int)b.FilterType,
                FrequencyHz = b.FrequencyHz,
                GainDb = b.GainDb,
                Q = b.Q
            };
        }

        return native;
    }
}

public sealed class AudioProfile
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string Name { get; set; } = "Flat (Reference)";
    public string Category { get; set; } = "Reference";
    public string Description { get; set; } = "Uncolored reference audio response.";
    public bool IsBuiltIn { get; set; }

    public float PreAmpDb { get; set; } = 0f;

    // Bass & Treble Boosters
    public bool BassEnabled { get; set; }
    public float BassBoostDb { get; set; } = 0f;
    public float BassFreqHz { get; set; } = 90f;
    public float BassExciterMix { get; set; } = 0f;

    public bool TrebleEnabled { get; set; }
    public float TrebleBoostDb { get; set; } = 0f;
    public float TrebleFreqHz { get; set; } = 6000f;
    public float ClarityBoostDb { get; set; } = 0f;

    // Multi-Band EQ
    public EqProfileModel Eq { get; set; } = new();

    // Loudness Normalization
    public bool LoudnessEnabled { get; set; }
    public float LoudnessTargetLufs { get; set; } = -15f;
    public float LoudnessStrength { get; set; } = 0.65f;
    public float LoudnessMaxGainDb { get; set; } = 9f;

    // Spatial Effects
    public bool WidenerEnabled { get; set; }
    public float StereoWidth { get; set; } = 1.0f;
    public float BassMonoFreqHz { get; set; } = 150f;

    public WaSurroundMode SurroundMode { get; set; } = WaSurroundMode.Off;
    public float SurroundDepth { get; set; } = 0.45f;

    public bool AmbienceEnabled { get; set; }
    public float AmbienceRoomSize { get; set; } = 0.45f;
    public float AmbienceDamping { get; set; } = 0.55f;
    public float AmbienceMix { get; set; } = 0.18f;

    // Controlled Sound Booster / Maximizer
    public bool MaximizerEnabled { get; set; } = true;
    public float MaximizerBoostDb { get; set; } = 0f;
    public float MaximizerCeilingDb { get; set; } = -0.3f;
    public float MaximizerSoftClip { get; set; } = 0.2f;
    public float MaximizerReleaseMs { get; set; } = 80f;

    public AudioProfile Clone(string? newName = null, bool asUserPreset = false)
    {
        return new AudioProfile
        {
            Id = asUserPreset ? Guid.NewGuid().ToString("N") : Id,
            Name = newName ?? Name,
            Category = asUserPreset ? "Custom" : Category,
            Description = Description,
            IsBuiltIn = !asUserPreset && IsBuiltIn,
            PreAmpDb = PreAmpDb,
            BassEnabled = BassEnabled,
            BassBoostDb = BassBoostDb,
            BassFreqHz = BassFreqHz,
            BassExciterMix = BassExciterMix,
            TrebleEnabled = TrebleEnabled,
            TrebleBoostDb = TrebleBoostDb,
            TrebleFreqHz = TrebleFreqHz,
            ClarityBoostDb = ClarityBoostDb,
            Eq = Eq.Clone(),
            LoudnessEnabled = LoudnessEnabled,
            LoudnessTargetLufs = LoudnessTargetLufs,
            LoudnessStrength = LoudnessStrength,
            LoudnessMaxGainDb = LoudnessMaxGainDb,
            WidenerEnabled = WidenerEnabled,
            StereoWidth = StereoWidth,
            BassMonoFreqHz = BassMonoFreqHz,
            SurroundMode = SurroundMode,
            SurroundDepth = SurroundDepth,
            AmbienceEnabled = AmbienceEnabled,
            AmbienceRoomSize = AmbienceRoomSize,
            AmbienceDamping = AmbienceDamping,
            AmbienceMix = AmbienceMix,
            MaximizerEnabled = MaximizerEnabled,
            MaximizerBoostDb = MaximizerBoostDb,
            MaximizerCeilingDb = MaximizerCeilingDb,
            MaximizerSoftClip = MaximizerSoftClip,
            MaximizerReleaseMs = MaximizerReleaseMs
        };
    }

    public WaDspParams ToNative(bool globalBypass)
    {
        return new WaDspParams
        {
            GlobalBypass = globalBypass ? 1 : 0,
            PreAmpDb = PreAmpDb,
            BassEnabled = BassEnabled ? 1 : 0,
            BassBoostDb = BassBoostDb,
            BassFreqHz = BassFreqHz,
            BassExciterMix = BassExciterMix,
            TrebleEnabled = TrebleEnabled ? 1 : 0,
            TrebleBoostDb = TrebleBoostDb,
            TrebleFreqHz = TrebleFreqHz,
            ClarityBoostDb = ClarityBoostDb,
            Eq = Eq.ToNative(),
            LoudnessEnabled = LoudnessEnabled ? 1 : 0,
            LoudnessTargetLufs = LoudnessTargetLufs,
            LoudnessStrength = LoudnessStrength,
            LoudnessMaxGainDb = LoudnessMaxGainDb,
            WidenerEnabled = WidenerEnabled ? 1 : 0,
            StereoWidth = StereoWidth,
            BassMonoFreqHz = BassMonoFreqHz,
            SurroundMode = (int)SurroundMode,
            SurroundDepth = SurroundDepth,
            AmbienceEnabled = AmbienceEnabled ? 1 : 0,
            AmbienceRoomSize = AmbienceRoomSize,
            AmbienceDamping = AmbienceDamping,
            AmbienceMix = AmbienceMix,
            MaximizerEnabled = MaximizerEnabled ? 1 : 0,
            MaximizerBoostDb = MaximizerBoostDb,
            MaximizerCeilingDb = MaximizerCeilingDb,
            MaximizerSoftClip = MaximizerSoftClip,
            MaximizerReleaseMs = MaximizerReleaseMs
        };
    }
}

public sealed class AppProfileRule
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string ProcessName { get; set; } = "";   // e.g. "Spotify" or "cs2" (without .exe)
    public string ProfileId { get; set; } = "";
    public bool Enabled { get; set; } = true;
}

public sealed class DeviceProfileMapping
{
    public string DeviceId { get; set; } = "";
    public string DeviceName { get; set; } = "";
    public string ProfileId { get; set; } = "";
}

public sealed class HotkeyBinding
{
    public string ActionId { get; set; } = "";      // "ToggleBypass", "CycleDevice", "ToggleMultiDevice", "NextPreset"
    public string DisplayName { get; set; } = "";
    public uint Modifiers { get; set; } = 0;        // 0 = unbound (opt-in!)
    public uint VirtualKey { get; set; } = 0;       // 0 = unbound
    public bool Enabled { get; set; } = false;

    [JsonIgnore]
    public bool IsBound => Enabled && VirtualKey != 0 && Modifiers != 0;
}

public sealed class SavedOutputSlot
{
    public string DeviceId { get; set; } = "";
    public string DeviceName { get; set; } = "";
    public bool Enabled { get; set; }
    public float Volume { get; set; } = 1.0f;
    public bool Mute { get; set; }
    public float Balance { get; set; } = 0f;
    public float SyncDelayMs { get; set; } = 0f;
    public string? CustomEqProfileId { get; set; }  // null = Follow Master
}

public sealed class AppConfig
{
    public string ActiveProfileId { get; set; } = "flat";
    public bool GlobalBypass { get; set; }
    public bool PreferVirtualSink { get; set; } = true;
    public string? PreferredVirtualSinkId { get; set; }
    public int BufferLatencyMs { get; set; } = 15;
    public bool MultiDeviceEnabled { get; set; }
    public bool AutoCalibrateOnDeviceAdd { get; set; } = true;
    public bool CalibrateVolumeLevel { get; set; } = false;
    public bool AutoProfileSwitchingEnabled { get; set; } = true;
    public bool ManualProfileLock { get; set; }
    public bool StartWithWindows { get; set; }
    public bool StartMinimizedToTray { get; set; }
    public bool MinimizeToTrayOnClose { get; set; } = true;

    // Opt-in Global Hotkeys (ALL unbound by default to avoid conflicts!)
    public bool GlobalHotkeysMasterEnabled { get; set; } = false;
    public List<HotkeyBinding> Hotkeys { get; set; } = CreateDefaultHotkeys();

    public List<AppProfileRule> AppRules { get; set; } = new();
    public List<DeviceProfileMapping> DeviceMappings { get; set; } = new();
    public List<SavedOutputSlot> OutputSlots { get; set; } = new();

    public static List<HotkeyBinding> CreateDefaultHotkeys() => new()
    {
        new() { ActionId = "ToggleBypass", DisplayName = "Toggle Quick Bypass", Modifiers = 0, VirtualKey = 0, Enabled = false },
        new() { ActionId = "CycleDevice", DisplayName = "Cycle Primary Output Device", Modifiers = 0, VirtualKey = 0, Enabled = false },
        new() { ActionId = "ToggleMultiDevice", DisplayName = "Toggle Multi-Device Output", Modifiers = 0, VirtualKey = 0, Enabled = false },
        new() { ActionId = "NextPreset", DisplayName = "Next Preset", Modifiers = 0, VirtualKey = 0, Enabled = false }
    };
}
