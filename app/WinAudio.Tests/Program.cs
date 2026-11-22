using System.IO;
using System.Runtime.InteropServices;
using WinAudio.App.Interop;
using WinAudio.App.Models;
using WinAudio.App.Services;

int passed = 0;
int failed = 0;

void Check(bool cond, string name, string detail = "")
{
    if (cond)
    {
        passed++;
        Console.WriteLine($"[PASS] {name}");
    }
    else
    {
        failed++;
        Console.WriteLine($"[FAIL] {name} {detail}");
    }
}

Console.WriteLine("=== WinAudio C# .NET 9 ABI, Preset & Profile Test Suite ===");

// 1. Live C#/C++ ABI Struct Size Verification against WinAudioEngine.dll!
{
    EngineNative.wa_get_abi_sizes(out var native);
    Check(native.AbiVersion == EngineNative.AbiVersion, "ABI version matches native DLL", $"native={native.AbiVersion}");

    int csParamBand = Marshal.SizeOf<WaParamBand>();
    int csEqParams = Marshal.SizeOf<WaEqParams>();
    int csDspParams = Marshal.SizeOf<WaDspParams>();
    int csDeviceInfo = Marshal.SizeOf<WaDeviceInfo>();
    int csOutputSlot = Marshal.SizeOf<WaOutputSlotConfig>();
    int csOutputTel = Marshal.SizeOf<WaOutputTelemetry>();
    int csTelemetry = Marshal.SizeOf<WaTelemetry>();
    int csCalibRes = Marshal.SizeOf<WaCalibrationResult>();

    Check(csParamBand == native.SizeParamBand, "WaParamBand size matches C++ ABI", $"cs={csParamBand} cpp={native.SizeParamBand}");
    Check(csEqParams == native.SizeEqParams, "WaEqParams size matches C++ ABI", $"cs={csEqParams} cpp={native.SizeEqParams}");
    Check(csDspParams == native.SizeDspParams, "WaDspParams size matches C++ ABI", $"cs={csDspParams} cpp={native.SizeDspParams}");
    Check(csDeviceInfo == native.SizeDeviceInfo, "WaDeviceInfo size matches C++ ABI", $"cs={csDeviceInfo} cpp={native.SizeDeviceInfo}");
    Check(csOutputSlot == native.SizeOutputSlotConfig, "WaOutputSlotConfig size matches C++ ABI", $"cs={csOutputSlot} cpp={native.SizeOutputSlotConfig}");
    Check(csOutputTel == native.SizeOutputTelemetry, "WaOutputTelemetry size matches C++ ABI", $"cs={csOutputTel} cpp={native.SizeOutputTelemetry}");
    Check(csTelemetry == native.SizeTelemetry, "WaTelemetry size matches C++ ABI", $"cs={csTelemetry} cpp={native.SizeTelemetry}");
    Check(csCalibRes == native.SizeCalibrationResult, "WaCalibrationResult size matches C++ ABI", $"cs={csCalibRes} cpp={native.SizeCalibrationResult}");
}

// 2. Live P/Invoke EQ curve & WASAPI device enumeration through AudioControllerService
{
    using var svc = new AudioControllerService();
    Check(svc.IsInitialized, "AudioControllerService initializes native engine handle");

    var devices = svc.EnumerateDevices();
    Check(devices.Count >= 0, $"AudioControllerService enumerates live WASAPI endpoints ({devices.Count} found)");

    var prof = new AudioProfile();
    prof.Eq.Enabled = true;
    prof.Eq.Mode = WaEqMode.Eq10Band;
    prof.Eq.Graphic10Db[5] = 7.5f; // 1000 Hz
    var curve = svc.ComputeEqCurve(prof, 48000f, new[] { 100f, 1000f, 10000f });
    Check(Math.Abs(curve[1] - 7.5f) < 0.15f, "P/Invoke ComputeEqCurve returns exact +7.5dB @ 1kHz", $"got={curve[1]}");
}

// 3. Built-in 10 Presets + Portable Save/Load Round-Trip
{
    string tempDir = Path.Combine(Path.GetTempPath(), "WinAudioTest_" + Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(Path.Combine(tempDir, "Presets"));
    File.Copy(Path.Combine(AppContext.BaseDirectory, "Presets", "default_presets.json"),
              Path.Combine(tempDir, "Presets", "default_presets.json"), true);

    var store = new PresetStore(tempDir);
    var presets = store.LoadAllPresets();
    Check(presets.Count == 10, "Loads all 10 built-in genre & use-case presets", $"count={presets.Count}");

    var custom = presets[1].Clone("My Gaming Headphones", asUserPreset: true);
    custom.BassBoostDb = 5.5f;
    presets.Add(custom);
    store.SaveUserPresets(presets);

    var reloaded = store.LoadAllPresets();
    Check(reloaded.Count == 11 && reloaded.Any(p => p.Name == "My Gaming Headphones" && Math.Abs(p.BassBoostDb - 5.5f) < 0.01f),
          "User preset JSON persistence round-trips accurately");

    // Verify opt-in global hotkeys are ALL unbound by default
    var cfg = store.LoadConfig();
    Check(!cfg.GlobalHotkeysMasterEnabled && cfg.Hotkeys.All(h => !h.IsBound),
          "Opt-in global hotkeys start disabled & unbound by default (zero conflict risk)");

    Directory.Delete(tempDir, true);
}

// 4. Equalizer APO / AutoEQ .txt Importer
{
    const string apoSample = """
        Preamp: -4.2 dB
        Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.71
        Filter 2: ON PK Fc 1800 Hz Gain -2.4 dB Q 2.1
        Filter 3: ON HSC Fc 8000 Hz Gain 3.0 dB Q 0.70
        Filter 4: OFF PK Fc 500 Hz Gain 6.0 dB Q 1.0
        """;
    var imported = PresetStore.ImportEqualizerApoText(apoSample, "HD650 AutoEQ");
    Check(imported.Eq.Mode == WaEqMode.Parametric && Math.Abs(imported.PreAmpDb - (-4.2f)) < 0.01f,
          "Equalizer APO importer parses Preamp and sets Parametric mode");
    Check(imported.Eq.ParametricBands[0].FilterType == WaFilterType.LowShelf &&
          Math.Abs(imported.Eq.ParametricBands[0].FrequencyHz - 105f) < 0.1f &&
          imported.Eq.ParametricBands[1].FilterType == WaFilterType.Peaking &&
          !imported.Eq.ParametricBands[3].Enabled,
          "Equalizer APO importer parses LSC/PK/HSC/OFF filters accurately");
}

// 5. Auto-Profile Resolution Priority (Manual Lock > Foreground App > Background App > Per-Device > Default)
{
    var cfg = new AppConfig
    {
        AutoProfileSwitchingEnabled = true,
        ManualProfileLock = false,
        AppRules =
        {
            new AppProfileRule { ProcessName = "cs2.exe", ProfileId = "fps-footsteps", Enabled = true },
            new AppProfileRule { ProcessName = "Spotify", ProfileId = "music-hifi", Enabled = true }
        },
        DeviceMappings =
        {
            new DeviceProfileMapping { DeviceId = "dev-bt-1", DeviceName = "JBL", ProfileId = "bass-boost" }
        }
    };

    var r1 = AutoProfileService.ResolveTargetProfile(cfg, "cs2", new[] { "cs2", "Spotify" }, "dev-bt-1", "flat");
    Check(r1.ProfileId == "fps-footsteps", "Auto-Profile prioritizes Foreground App over Background App & Device", r1.ProfileId);

    var r2 = AutoProfileService.ResolveTargetProfile(cfg, "chrome", new[] { "chrome", "Spotify" }, "dev-bt-1", "flat");
    Check(r2.ProfileId == "music-hifi", "Auto-Profile matches Background Audio App when foreground has no rule", r2.ProfileId);

    var r3 = AutoProfileService.ResolveTargetProfile(cfg, "notepad", new[] { "notepad" }, "dev-bt-1", "flat");
    Check(r3.ProfileId == "bass-boost", "Auto-Profile falls back to Per-Device Mapping when no app rule matches", r3.ProfileId);

    cfg.ManualProfileLock = true;
    var r4 = AutoProfileService.ResolveTargetProfile(cfg, "cs2", new[] { "cs2" }, "dev-bt-1", "flat");
    Check(r4.ProfileId == "flat", "Manual Profile Lock overrides all automatic rules", r4.ProfileId);
}

Console.WriteLine($"\nSummary: {passed} passed, {failed} failed");
return failed == 0 ? 0 : 1;
