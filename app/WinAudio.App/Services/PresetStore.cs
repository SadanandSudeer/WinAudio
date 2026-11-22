using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Text.RegularExpressions;
using WinAudio.App.Interop;
using WinAudio.App.Models;

namespace WinAudio.App.Services;

public sealed class PresetStore
{
    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true
    };

    private readonly string _baseDir;
    private readonly string _configPath;
    private readonly string _userPresetsPath;
    private readonly string _defaultPresetsPath;

    public PresetStore(string? baseDir = null)
    {
        _baseDir = baseDir ?? AppContext.BaseDirectory;
        Directory.CreateDirectory(Path.Combine(_baseDir, "Data"));
        _configPath = Path.Combine(_baseDir, "Data", "config.json");
        _userPresetsPath = Path.Combine(_baseDir, "Data", "user_presets.json");
        _defaultPresetsPath = Path.Combine(_baseDir, "Presets", "default_presets.json");
    }

    public List<AudioProfile> LoadAllPresets()
    {
        var all = new List<AudioProfile>();

        if (File.Exists(_defaultPresetsPath))
        {
            try
            {
                var json = File.ReadAllText(_defaultPresetsPath);
                var builtIns = JsonSerializer.Deserialize<List<AudioProfile>>(json, JsonOpts);
                if (builtIns != null)
                {
                    foreach (var p in builtIns)
                    {
                        NormalizeProfile(p);
                        p.IsBuiltIn = true;
                        all.Add(p);
                    }
                }
            }
            catch { /* fall back to code default */ }
        }

        if (all.Count == 0)
        {
            all.Add(new AudioProfile { Id = "flat", Name = "Flat (Reference)", IsBuiltIn = true });
        }

        if (File.Exists(_userPresetsPath))
        {
            try
            {
                var json = File.ReadAllText(_userPresetsPath);
                var users = JsonSerializer.Deserialize<List<AudioProfile>>(json, JsonOpts);
                if (users != null)
                {
                    foreach (var p in users)
                    {
                        NormalizeProfile(p);
                        p.IsBuiltIn = false;
                        all.Add(p);
                    }
                }
            }
            catch { }
        }

        return all;
    }

    public void SaveUserPresets(IEnumerable<AudioProfile> allPresets)
    {
        var userOnly = allPresets.Where(p => !p.IsBuiltIn).ToList();
        var json = JsonSerializer.Serialize(userOnly, JsonOpts);
        File.WriteAllText(_userPresetsPath, json);
    }

    public AppConfig LoadConfig()
    {
        if (File.Exists(_configPath))
        {
            try
            {
                var json = File.ReadAllText(_configPath);
                var cfg = JsonSerializer.Deserialize<AppConfig>(json, JsonOpts);
                if (cfg != null)
                {
                    EnsureHotkeyActions(cfg);
                    return cfg;
                }
            }
            catch { }
        }
        return new AppConfig();
    }

    public void SaveConfig(AppConfig config)
    {
        var json = JsonSerializer.Serialize(config, JsonOpts);
        File.WriteAllText(_configPath, json);
    }

    public void ExportPreset(AudioProfile profile, string filePath)
    {
        var json = JsonSerializer.Serialize(profile, JsonOpts);
        File.WriteAllText(filePath, json);
    }

    public AudioProfile? ImportPresetFromFile(string filePath)
    {
        var text = File.ReadAllText(filePath);
        if (filePath.EndsWith(".txt", StringComparison.OrdinalIgnoreCase))
        {
            return ImportEqualizerApoText(text, Path.GetFileNameWithoutExtension(filePath));
        }
        var p = JsonSerializer.Deserialize<AudioProfile>(text, JsonOpts);
        if (p == null) return null;
        NormalizeProfile(p);
        p.Id = Guid.NewGuid().ToString("N");
        p.IsBuiltIn = false;
        return p;
    }

    // Parses standard Equalizer APO / AutoEQ .txt parametric filter files into a WinAudio Semi-Parametric profile
    public static AudioProfile ImportEqualizerApoText(string content, string name = "Imported AutoEQ")
    {
        var profile = new AudioProfile
        {
            Id = Guid.NewGuid().ToString("N"),
            Name = string.IsNullOrWhiteSpace(name) ? "Imported AutoEQ" : name,
            Category = "Imported",
            Description = "Imported from Equalizer APO / AutoEQ filter specification.",
            IsBuiltIn = false,
            MaximizerEnabled = true
        };
        profile.Eq.Enabled = true;
        profile.Eq.Mode = WaEqMode.Parametric;
        foreach (var b in profile.Eq.ParametricBands) b.Enabled = false;

        int bandIdx = 0;
        var preampRe = new Regex(@"Preamp:\s*([+-]?\d+(?:\.\d+)?)\s*dB", RegexOptions.IgnoreCase);
        var filterRe = new Regex(
            @"Filter\s*\d*:\s*(ON|OFF)\s+(PK|LSC|LS|HSC|HS|LP|LPQ|HP|HPQ|NO)\s+Fc\s+(\d+(?:\.\d+)?)\s*Hz(?:\s+Gain\s+([+-]?\d+(?:\.\d+)?)\s*dB)?(?:\s+Q\s+(\d+(?:\.\d+)?))?",
            RegexOptions.IgnoreCase);

        foreach (var rawLine in content.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries))
        {
            var line = rawLine.Trim();
            if (line.StartsWith('#')) continue;

            var pm = preampRe.Match(line);
            if (pm.Success && float.TryParse(pm.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out float pre))
            {
                profile.PreAmpDb = Math.Clamp(pre, -18f, 24f);
                continue;
            }

            if (bandIdx >= EngineNative.MaxParamBands) continue;
            var fm = filterRe.Match(line);
            if (!fm.Success) continue;

            bool on = fm.Groups[1].Value.Equals("ON", StringComparison.OrdinalIgnoreCase);
            string typeStr = fm.Groups[2].Value.ToUpperInvariant();
            WaFilterType ftype = typeStr switch
            {
                "LSC" or "LS" => WaFilterType.LowShelf,
                "HSC" or "HS" => WaFilterType.HighShelf,
                "LP" or "LPQ" => WaFilterType.LowPass,
                "HP" or "HPQ" => WaFilterType.HighPass,
                "NO" => WaFilterType.Notch,
                _ => WaFilterType.Peaking
            };

            float fc = float.Parse(fm.Groups[3].Value, CultureInfo.InvariantCulture);
            float gain = fm.Groups[4].Success ? float.Parse(fm.Groups[4].Value, CultureInfo.InvariantCulture) : 0f;
            float q = fm.Groups[5].Success ? float.Parse(fm.Groups[5].Value, CultureInfo.InvariantCulture) : 1.0f;

            var band = profile.Eq.ParametricBands[bandIdx++];
            band.Enabled = on;
            band.FilterType = ftype;
            band.FrequencyHz = Math.Clamp(fc, 20f, 20000f);
            band.GainDb = Math.Clamp(gain, -18f, 18f);
            band.Q = Math.Clamp(q, 0.3f, 12f);
        }

        return profile;
    }

    private static void NormalizeProfile(AudioProfile p)
    {
        p.Eq ??= new EqProfileModel();
        if (p.Eq.Graphic10Db == null || p.Eq.Graphic10Db.Length != 10)
            p.Eq.Graphic10Db = new float[10];
        if (p.Eq.Graphic31Db == null || p.Eq.Graphic31Db.Length != 31)
            p.Eq.Graphic31Db = new float[31];
        if (p.Eq.ParametricBands == null || p.Eq.ParametricBands.Count != EngineNative.MaxParamBands)
            p.Eq.ParametricBands = EqProfileModel.CreateDefaultParametric();
    }

    private static void EnsureHotkeyActions(AppConfig cfg)
    {
        cfg.Hotkeys ??= AppConfig.CreateDefaultHotkeys();
        foreach (var def in AppConfig.CreateDefaultHotkeys())
        {
            if (!cfg.Hotkeys.Any(h => h.ActionId == def.ActionId))
                cfg.Hotkeys.Add(def);
        }
    }
}
