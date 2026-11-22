using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Threading;
using WinAudio.App.Interop;
using WinAudio.App.Models;
using WinAudio.App.Services;

namespace WinAudio.App.ViewModels;

public sealed class GraphicBandItem : INotifyPropertyChanged
{
    private float _gainDb;
    public int Index { get; init; }
    public string Label { get; init; } = "";
    public float FrequencyHz { get; init; }

    public float GainDb
    {
        get => _gainDb;
        set
        {
            float clamped = Math.Clamp(value, -15f, 15f);
            if (Math.Abs(_gainDb - clamped) > 0.01f)
            {
                _gainDb = clamped;
                OnPropertyChanged();
                OnPropertyChanged(nameof(GainText));
                Changed?.Invoke(Index, clamped);
            }
        }
    }

    public void SetSilent(float v)
    {
        _gainDb = Math.Clamp(v, -15f, 15f);
        OnPropertyChanged(nameof(GainDb));
        OnPropertyChanged(nameof(GainText));
    }

    public string GainText => $"{GainDb:+0.0;-0.0;0.0}";
    public event Action<int, float>? Changed;
    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? n = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
}

public sealed class HotkeyRowViewModel : INotifyPropertyChanged
{
    private string _statusText = "Unbound (Opt-In)";
    private bool _hasConflict;

    public HotkeyBinding Model { get; }
    public string ActionId => Model.ActionId;
    public string DisplayName => Model.DisplayName;
    public string ShortcutText => GlobalHotkeyService.FormatBinding(Model);

    public string StatusText
    {
        get => _statusText;
        set { _statusText = value; OnPropertyChanged(); }
    }

    public bool HasConflict
    {
        get => _hasConflict;
        set { _hasConflict = value; OnPropertyChanged(); }
    }

    public HotkeyRowViewModel(HotkeyBinding model) => Model = model;

    public void RefreshShortcutText()
    {
        OnPropertyChanged(nameof(ShortcutText));
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? n = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
}

public sealed class MainViewModel : INotifyPropertyChanged, IDisposable
{
    private readonly AudioControllerService _engine;
    private readonly PresetStore _store;
    private readonly GlobalHotkeyService _hotkeys;
    private readonly DispatcherTimer _telemetryTimer;
    private readonly DispatcherTimer _autoProfileTimer;
    private bool _suppressPush;
    private bool _uiVisible = true;
    private string _activeSection = "Dashboard";
    private string _activeAutoProfileStatus = "Manual";
    private string _statusBanner = "";
    private WaRoutingMode _routingMode = WaRoutingMode.Stopped;
    private int _engineSampleRate = 48000;
    private float _cpuLoadPercent;
    private double _inPeakL = -90, _inPeakR = -90, _outPeakL = -90, _outPeakR = -90;
    private double _limiterGrDb, _loudnessGainDb;
    private bool _testToneEnabled;
    private int _selectedParamBandIndex;
    private bool _hasMicrophone;
    private string _microphoneName = "";
    private bool _isCalibrating;
    private string _calibrationStatusText = "Enable 2+ devices to calibrate sync delay & levels using your microphone.";

    public AppConfig Config { get; }
    public AudioProfile CurrentProfile { get; private set; }
    public ObservableCollection<AudioProfile> Presets { get; } = new();
    public ObservableCollection<DeviceViewModel> Devices { get; } = new();
    public ObservableCollection<DeviceViewModel> PhysicalDevices { get; } = new();
    public ObservableCollection<DeviceViewModel> VirtualDevices { get; } = new();
    public ObservableCollection<GraphicBandItem> Eq10Bands { get; } = new();
    public ObservableCollection<GraphicBandItem> Eq31Bands { get; } = new();
    public ObservableCollection<AppProfileRule> AppRules { get; } = new();
    public ObservableCollection<DeviceProfileMapping> DeviceMappings { get; } = new();
    public ObservableCollection<HotkeyRowViewModel> HotkeyRows { get; } = new();
    public ObservableCollection<string> RunningApps { get; } = new();

    public event Action<AudioProfile, float[]?, float[]?>? EqVisualsUpdated;
    public event Action<float[]>? SpectrumUpdated;
    public event Action? TrayRefreshRequested;

    public MainViewModel(AudioControllerService engine, PresetStore store, GlobalHotkeyService hotkeys)
    {
        _engine = engine;
        _store = store;
        _hotkeys = hotkeys;

        Config = _store.LoadConfig();
        Config.StartWithWindows = StartupService.IsAutoStartEnabled();

        foreach (var p in _store.LoadAllPresets()) Presets.Add(p);
        foreach (var r in Config.AppRules) AppRules.Add(r);
        foreach (var m in Config.DeviceMappings) DeviceMappings.Add(m);
        foreach (var h in Config.Hotkeys) HotkeyRows.Add(new HotkeyRowViewModel(h));

        var initial = Presets.FirstOrDefault(p => p.Id == Config.ActiveProfileId) ?? Presets[0];
        CurrentProfile = initial.Clone();

        InitGraphicBandCollections();
        SyncBandsFromProfile();

        _engine.DevicesChanged += () => Application.Current?.Dispatcher.BeginInvoke(RefreshDevices);
        _hotkeys.HotkeyTriggered += OnGlobalHotkey;

        RefreshDevices();
        StartEngine();

        _telemetryTimer = new DispatcherTimer(DispatcherPriority.Background)
        {
            Interval = TimeSpan.FromMilliseconds(50) // 20 Hz smooth meters/spectrum when window visible
        };
        _telemetryTimer.Tick += (_, _) => PollTelemetry();
        _telemetryTimer.Start();

        _autoProfileTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(1500)
        };
        _autoProfileTimer.Tick += (_, _) => EvaluateAutoProfile();
        _autoProfileTimer.Start();
    }

    private void InitGraphicBandCollections()
    {
        string[] labels10 = { "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
        float[] f10 = { 31.25f, 62.5f, 125f, 250f, 500f, 1000f, 2000f, 4000f, 8000f, 16000f };
        for (int i = 0; i < 10; i++)
        {
            var item = new GraphicBandItem { Index = i, Label = labels10[i], FrequencyHz = f10[i] };
            item.Changed += (idx, g) =>
            {
                if (_suppressPush) return;
                CurrentProfile.Eq.Graphic10Db[idx] = g;
                PushDspToEngine();
            };
            Eq10Bands.Add(item);
        }

        string[] labels31 = {
            "20","25","31","40","50","63","80","100","125","160",
            "200","250","315","400","500","630","800","1k","1.2k","1.6k",
            "2k","2.5k","3.1k","4k","5k","6.3k","8k","10k","12.5k","16k","20k"
        };
        float[] f31 = {
            20,25,31.5f,40,50,63,80,100,125,160,
            200,250,315,400,500,630,800,1000,1250,1600,
            2000,2500,3150,4000,5000,6300,8000,10000,12500,16000,20000
        };
        for (int i = 0; i < 31; i++)
        {
            var item = new GraphicBandItem { Index = i, Label = labels31[i], FrequencyHz = f31[i] };
            item.Changed += (idx, g) =>
            {
                if (_suppressPush) return;
                CurrentProfile.Eq.Graphic31Db[idx] = g;
                PushDspToEngine();
            };
            Eq31Bands.Add(item);
        }
    }

    private void SyncBandsFromProfile()
    {
        _suppressPush = true;
        for (int i = 0; i < 10; i++) Eq10Bands[i].SetSilent(CurrentProfile.Eq.Graphic10Db[i]);
        for (int i = 0; i < 31; i++) Eq31Bands[i].SetSilent(CurrentProfile.Eq.Graphic31Db[i]);
        _suppressPush = false;
    }

    public void SetUiVisibility(bool visible)
    {
        _uiVisible = visible;
        if (visible)
        {
            if (!_telemetryTimer.IsEnabled) _telemetryTimer.Start();
            RequestCurveRedraw();
        }
        else
        {
            // Suspend telemetry & FFT polling completely when minimized to tray -> ~0% UI CPU!
            _telemetryTimer.Stop();
        }
    }

    public void InitializeHotkeys(IntPtr windowHandle)
    {
        _hotkeys.Initialize(windowHandle);
        ApplyHotkeys();
    }

    public void ApplyHotkeys()
    {
        var results = _hotkeys.ApplyBindings(Config.GlobalHotkeysMasterEnabled, Config.Hotkeys);
        foreach (var row in HotkeyRows)
        {
            row.RefreshShortcutText();
            if (!Config.GlobalHotkeysMasterEnabled)
            {
                row.HasConflict = false;
                row.StatusText = row.Model.IsBound ? "Disabled (Master Off)" : "Unbound (Opt-In)";
            }
            else if (!row.Model.IsBound)
            {
                row.HasConflict = false;
                row.StatusText = "Unbound";
            }
            else if (results.TryGetValue(row.ActionId, out bool ok) && ok)
            {
                row.HasConflict = false;
                row.StatusText = "Active";
            }
            else
            {
                row.HasConflict = true;
                row.StatusText = "Conflict! In use by another app";
            }
        }
        SaveConfigSafe();
    }

    private void OnGlobalHotkey(string actionId)
    {
        Application.Current?.Dispatcher.Invoke(() =>
        {
            switch (actionId)
            {
                case "ToggleBypass":
                    GlobalBypass = !GlobalBypass;
                    break;
                case "CycleDevice":
                    CycleOutputDevice();
                    break;
                case "ToggleMultiDevice":
                    MultiDeviceEnabled = !MultiDeviceEnabled;
                    break;
                case "NextPreset":
                    CycleNextPreset();
                    break;
            }
        });
    }

    public void StartEngine()
    {
        _engine.Start(Config.PreferredVirtualSinkId, Config.PreferVirtualSink, Config.BufferLatencyMs);
        RoutingMode = _engine.GetRoutingMode();
        SyncDirectLoopbackDeviceStates();
        PushDspToEngine();
        ApplyOutputsToEngine();
        NotifyTray();
    }

    private void SyncDirectLoopbackDeviceStates()
    {
        _suppressPush = true;
        foreach (var d in PhysicalDevices)
        {
            bool isLoopbackSrc = (RoutingMode == WaRoutingMode.DirectLoopback && d.IsDefault);
            d.IsDirectLoopbackSource = isLoopbackSrc;
            if (isLoopbackSrc && _engine.TryGetEndpointVolume(d.Id, out float vol, out bool muted, out float bal))
            {
                d.SetVolumeMuteBalanceSilent(vol, muted, bal);
            }
        }
        _suppressPush = false;
    }

    public void RestartEngine()
    {
        _engine.Stop();
        StartEngine();
    }

    public void RefreshDevices()
    {
        var raw = _engine.EnumerateDevices();
        var prevSlots = Config.OutputSlots.ToDictionary(s => s.DeviceId, StringComparer.OrdinalIgnoreCase);

        Devices.Clear();
        PhysicalDevices.Clear();
        VirtualDevices.Clear();

        foreach (var d in raw)
        {
            var vm = new DeviceViewModel
            {
                Id = d.Id ?? "",
                Name = d.Name ?? "Unknown Device",
                Transport = (WaDeviceTransport)d.Transport,
                IsDefault = d.IsDefault != 0,
                IsDefaultComm = d.IsDefaultComm != 0,
                IsVirtualSink = d.IsVirtualSink != 0,
                IsBluetoothHfp = d.IsBluetoothHfp != 0,
                SampleRate = d.SampleRate > 0 ? d.SampleRate : 48000,
                Channels = d.Channels > 0 ? d.Channels : 2
            };

            if (prevSlots.TryGetValue(vm.Id, out var saved))
            {
                vm.IsMultiEnabled = saved.Enabled;
                vm.Volume = saved.Volume;
                vm.Mute = saved.Mute;
                vm.Balance = saved.Balance;
                vm.SyncDelayMs = saved.SyncDelayMs;
                vm.CustomEqProfileId = saved.CustomEqProfileId;
            }
            else if (vm.IsDefault && !vm.IsVirtualSink)
            {
                vm.IsMultiEnabled = true;
            }

            vm.PropertyChanged += (_, e) =>
            {
                if (_suppressPush) return;
                if (e.PropertyName is nameof(DeviceViewModel.Volume) or nameof(DeviceViewModel.Mute) or nameof(DeviceViewModel.Balance))
                {
                    if (vm.IsDirectLoopbackSource)
                    {
                        // Control the Windows Default Device directly via Core Audio IAudioEndpointVolume
                        _engine.SetEndpointVolume(vm.Id, vm.Volume, vm.Mute, vm.Balance);
                        return;
                    }
                    ApplyOutputsToEngine();
                    PersistOutputSlots();
                }
                else if (e.PropertyName is nameof(DeviceViewModel.IsMultiEnabled))
                {
                    if (vm.IsDirectLoopbackSource)
                    {
                        vm.Mute = !vm.IsMultiEnabled;
                        return;
                    }
                    ApplyOutputsToEngine();
                    PersistOutputSlots();
                    OnPropertyChanged(nameof(CanCalibrateNow));

                    // Auto-trigger acoustic mic calibration when an additional device is enabled (only if a mic is attached!)
                    if (vm.IsMultiEnabled && Config.MultiDeviceEnabled && Config.AutoCalibrateOnDeviceAdd && HasMicrophone && !_isCalibrating)
                    {
                        int enabledCount = PhysicalDevices.Count(x => x.IsMultiEnabled || x.IsDirectLoopbackSource);
                        if (enabledCount >= 2)
                        {
                            _ = RunAutoCalibrationAsync(initialDelayMs: 650);
                        }
                    }
                }
                else if (e.PropertyName is nameof(DeviceViewModel.SyncDelayMs) or
                         nameof(DeviceViewModel.CustomEqProfileId))
                {
                    ApplyOutputsToEngine();
                    PersistOutputSlots();
                }
            };

            Devices.Add(vm);
            if (vm.IsVirtualSink) VirtualDevices.Add(vm);
            else PhysicalDevices.Add(vm);
        }

        // Ensure at least one physical device is checked if none saved
        if (PhysicalDevices.Count > 0 && !PhysicalDevices.Any(x => x.IsMultiEnabled))
        {
            var primary = PhysicalDevices.FirstOrDefault(x => x.IsDefault) ?? PhysicalDevices[0];
            primary.IsMultiEnabled = true;
        }

        SyncDirectLoopbackDeviceStates();
        RefreshMicrophoneStatus();

        OnPropertyChanged(nameof(HasVirtualSink));
        OnPropertyChanged(nameof(PrimaryDeviceName));
        OnPropertyChanged(nameof(ActiveOutputSummary));
        OnPropertyChanged(nameof(RoutingModeBadge));
        OnPropertyChanged(nameof(RoutingModeDetail));
        NotifyTray();
    }

    public void SwitchDefaultDevice(string deviceId)
    {
        if (string.IsNullOrWhiteSpace(deviceId)) return;
        _engine.SetDefaultDevice(deviceId, true);

        // If single-device mode (MultiDeviceEnabled == false), also route single output slot to this device
        if (!Config.MultiDeviceEnabled)
        {
            foreach (var d in PhysicalDevices)
            {
                d.IsMultiEnabled = d.Id.Equals(deviceId, StringComparison.OrdinalIgnoreCase);
            }
        }
        RefreshDevices();
        ApplyOutputsToEngine();
        EvaluateAutoProfile();
    }

    public void CycleOutputDevice()
    {
        if (PhysicalDevices.Count < 2) return;
        int curIdx = -1;
        for (int i = 0; i < PhysicalDevices.Count; i++)
        {
            if (PhysicalDevices[i].IsDefault) { curIdx = i; break; }
        }
        int nextIdx = (curIdx + 1) % PhysicalDevices.Count;
        SwitchDefaultDevice(PhysicalDevices[nextIdx].Id);
    }

    public void ApplyOutputsToEngine()
    {
        IEnumerable<DeviceViewModel> targets;
        if (Config.MultiDeviceEnabled)
        {
            targets = PhysicalDevices.Where(d => d.IsMultiEnabled);
        }
        else
        {
            var single = PhysicalDevices.FirstOrDefault(d => d.IsMultiEnabled)
                      ?? PhysicalDevices.FirstOrDefault(d => d.IsDefault)
                      ?? PhysicalDevices.FirstOrDefault();
            targets = single != null ? new[] { single } : Array.Empty<DeviceViewModel>();
        }

        _engine.ConfigureOutputs(targets, Presets);
        OnPropertyChanged(nameof(ActiveOutputSummary));
        NotifyTray();
    }

    private void PersistOutputSlots()
    {
        Config.OutputSlots = PhysicalDevices.Select(d => new SavedOutputSlot
        {
            DeviceId = d.Id,
            DeviceName = d.Name,
            Enabled = d.IsMultiEnabled,
            Volume = d.Volume,
            Mute = d.Mute,
            Balance = d.Balance,
            SyncDelayMs = d.SyncDelayMs,
            CustomEqProfileId = d.CustomEqProfileId
        }).ToList();
        SaveConfigSafe();
    }

    public void SelectPreset(string profileId, string reason = "Manual")
    {
        var found = Presets.FirstOrDefault(p => p.Id == profileId);
        if (found == null) return;

        CurrentProfile = found.Clone();
        Config.ActiveProfileId = found.Id;
        ActiveAutoProfileStatus = reason;

        SyncBandsFromProfile();
        NotifyAllDspProperties();
        PushDspToEngine();
        SaveConfigSafe();
        NotifyTray();
    }

    public void CycleNextPreset()
    {
        if (Presets.Count == 0) return;
        int idx = Presets.ToList().FindIndex(p => p.Id == CurrentProfile.Id);
        int next = (idx + 1) % Presets.Count;
        SelectPreset(Presets[next].Id, "Manual");
    }

    public AudioProfile SaveCurrentAsUserPreset(string name)
    {
        string cleanName = string.IsNullOrWhiteSpace(name) ? $"Custom Preset {Presets.Count + 1}" : name.Trim();
        var saved = CurrentProfile.Clone(cleanName, asUserPreset: true);
        Presets.Add(saved);
        _store.SaveUserPresets(Presets);
        SelectPreset(saved.Id, "Saved");
        return saved;
    }

    public void DeleteUserPreset(string profileId)
    {
        var target = Presets.FirstOrDefault(p => p.Id == profileId && !p.IsBuiltIn);
        if (target == null) return;
        Presets.Remove(target);
        _store.SaveUserPresets(Presets);
        if (CurrentProfile.Id == profileId) SelectPreset("flat");
    }

    public void ImportPreset(string filePath)
    {
        var imported = _store.ImportPresetFromFile(filePath);
        if (imported == null) return;
        Presets.Add(imported);
        _store.SaveUserPresets(Presets);
        SelectPreset(imported.Id, "Imported");
    }

    public void ExportCurrentPreset(string filePath) => _store.ExportPreset(CurrentProfile, filePath);

    public void FlattenEq()
    {
        Array.Clear(CurrentProfile.Eq.Graphic10Db);
        Array.Clear(CurrentProfile.Eq.Graphic31Db);
        foreach (var b in CurrentProfile.Eq.ParametricBands) b.GainDb = 0f;
        SyncBandsFromProfile();
        OnPropertyChanged(nameof(SelectedParamBand));
        PushDspToEngine();
    }

    public void HandleCanvasBandDrag(int index, float freqHz, float gainDb, float q)
    {
        if (CurrentProfile.Eq.Mode == WaEqMode.Eq10Band && index < 10)
        {
            CurrentProfile.Eq.Graphic10Db[index] = gainDb;
            Eq10Bands[index].SetSilent(gainDb);
        }
        else if (CurrentProfile.Eq.Mode == WaEqMode.Eq31Band && index < 31)
        {
            CurrentProfile.Eq.Graphic31Db[index] = gainDb;
            Eq31Bands[index].SetSilent(gainDb);
        }
        else if (CurrentProfile.Eq.Mode == WaEqMode.Parametric && index < CurrentProfile.Eq.ParametricBands.Count)
        {
            var b = CurrentProfile.Eq.ParametricBands[index];
            b.Enabled = true;
            b.FrequencyHz = freqHz;
            b.GainDb = gainDb;
            b.Q = q;
            SelectedParamBandIndex = index;
            OnPropertyChanged(nameof(SelectedParamBand));
        }
        PushDspToEngine();
    }

    public void PushDspToEngine()
    {
        _engine.ApplyProfile(CurrentProfile, Config.GlobalBypass);
        RequestCurveRedraw();
    }

    private float[]? _curveFreqCache;
    public void RegisterCurveFrequencies(float[] freqs) => _curveFreqCache = freqs;

    public void RequestCurveRedraw()
    {
        if (!_uiVisible || _curveFreqCache == null) return;
        var curve = _engine.ComputeEqCurve(CurrentProfile, Math.Max(44100f, _engineSampleRate), _curveFreqCache);
        EqVisualsUpdated?.Invoke(CurrentProfile, curve, null);
    }

    public void TriggerSyncPulse() => _engine.TriggerSyncPulse();

    public void RefreshMicrophoneStatus()
    {
        bool hasMic = _engine.TryGetMicrophone(out string micName);
        HasMicrophone = hasMic;
        MicrophoneName = hasMic ? micName : "";
        OnPropertyChanged(nameof(MicStatusText));
        OnPropertyChanged(nameof(CanCalibrateNow));
    }

    public async Task RunAutoCalibrationAsync(int initialDelayMs = 0)
    {
        RefreshMicrophoneStatus();
        if (!HasMicrophone)
        {
            CalibrationStatusText = "Calibration unavailable: no active microphone detected on this system.";
            return;
        }
        if (_isCalibrating) return;

        var activeDevices = PhysicalDevices
            .Where(d => d.IsMultiEnabled || d.IsDirectLoopbackSource)
            .ToList();

        if (activeDevices.Count < 2)
        {
            CalibrationStatusText = "Enable at least 2 output devices to run acoustic sync calibration.";
            return;
        }

        IsCalibrating = true;
        CalibrationStatusText = $"Calibrating {activeDevices.Count} devices via {MicrophoneName}... Please keep room quiet (~{activeDevices.Count}s).";

        try
        {
            if (initialDelayMs > 0) await Task.Delay(initialDelayMs);

            var ids = activeDevices.Select(d => d.Id).ToList();
            var results = await _engine.RunMicCalibrationAsync(ids);

            int okCount = results.Count(r => r.Succeeded != 0);
            if (okCount < 2)
            {
                CalibrationStatusText = $"Mic only heard {okCount}/{activeDevices.Count} devices clearly. Turn speakers up or hold headphones closer to {MicrophoneName} and retry.";
                return;
            }

            var summaryParts = new List<string>();
            string? defaultNeedsDelayHint = null;

            foreach (var r in results)
            {
                var dev = PhysicalDevices.FirstOrDefault(d => d.Id.Equals(r.DeviceId, StringComparison.OrdinalIgnoreCase));
                if (dev == null) continue;

                if (r.Succeeded == 0)
                {
                    summaryParts.Add($"{dev.Name}: not heard");
                    continue;
                }

                if (dev.IsDirectLoopbackSource)
                {
                    if (r.RecommendedDelayMs >= 4f)
                    {
                        var slowest = results.Where(x => x.Succeeded != 0).OrderByDescending(x => x.MeasuredLatencyMs).FirstOrDefault();
                        var slowDev = PhysicalDevices.FirstOrDefault(d => d.Id.Equals(slowest.DeviceId, StringComparison.OrdinalIgnoreCase));
                        defaultNeedsDelayHint = $" Tip: {dev.Name} (Default) is ~{r.RecommendedDelayMs:0} ms ahead of {slowDev?.Name ?? "slowest"} — set {slowDev?.Name ?? "it"} as Windows Default (or install VB-CABLE) to delay {dev.Name}.";
                    }
                    summaryParts.Add($"{dev.Name}: {r.MeasuredLatencyMs:0}ms");
                }
                else
                {
                    dev.SyncDelayMs = Math.Clamp(r.RecommendedDelayMs, 0f, 500f);
                    if (Config.CalibrateVolumeLevel)
                    {
                        dev.Volume = Math.Clamp(r.RecommendedVolume, 0.35f, 1.35f);
                    }
                    summaryParts.Add($"{dev.Name}: +{dev.SyncDelayMs:0}ms");
                }
            }

            ApplyOutputsToEngine();
            PersistOutputSlots();
            CalibrationStatusText = $"Synced! {string.Join("  |  ", summaryParts)}.{defaultNeedsDelayHint ?? ""}";
        }
        finally
        {
            IsCalibrating = false;
        }
    }

    public void RefreshRunningApps()
    {
        RunningApps.Clear();
        foreach (var name in AutoProfileService.GetRunningAppNames()) RunningApps.Add(name);
    }

    public void AddAppRule(string processName, string profileId)
    {
        if (string.IsNullOrWhiteSpace(processName) || string.IsNullOrWhiteSpace(profileId)) return;
        var rule = new AppProfileRule { ProcessName = processName.Trim(), ProfileId = profileId, Enabled = true };
        AppRules.Add(rule);
        Config.AppRules = AppRules.ToList();
        SaveConfigSafe();
    }

    public void RemoveAppRule(AppProfileRule rule)
    {
        AppRules.Remove(rule);
        Config.AppRules = AppRules.ToList();
        SaveConfigSafe();
    }

    public void SetDeviceProfileMapping(string deviceId, string deviceName, string? profileId)
    {
        var existing = DeviceMappings.FirstOrDefault(m => m.DeviceId.Equals(deviceId, StringComparison.OrdinalIgnoreCase));
        if (string.IsNullOrWhiteSpace(profileId))
        {
            if (existing != null) DeviceMappings.Remove(existing);
        }
        else if (existing != null)
        {
            existing.ProfileId = profileId;
        }
        else
        {
            DeviceMappings.Add(new DeviceProfileMapping { DeviceId = deviceId, DeviceName = deviceName, ProfileId = profileId });
        }
        Config.DeviceMappings = DeviceMappings.ToList();
        SaveConfigSafe();
    }

    private void EvaluateAutoProfile()
    {
        if (!Config.AutoProfileSwitchingEnabled || Config.ManualProfileLock) return;
        if (Config.AppRules.Count == 0 && Config.DeviceMappings.Count == 0) return;

        string? fg = AutoProfileService.GetForegroundProcessName();
        var running = AutoProfileService.GetRunningAppNames();
        string? primaryDevId = PhysicalDevices.FirstOrDefault(d => d.IsDefault || d.IsMultiEnabled)?.Id;

        var res = AutoProfileService.ResolveTargetProfile(Config, fg, running, primaryDevId, Config.ActiveProfileId);
        if (!string.IsNullOrWhiteSpace(res.ProfileId) && res.ProfileId != CurrentProfile.Id)
        {
            var found = Presets.FirstOrDefault(p => p.Id == res.ProfileId);
            if (found != null)
            {
                CurrentProfile = found.Clone();
                ActiveAutoProfileStatus = res.Reason;
                SyncBandsFromProfile();
                NotifyAllDspProperties();
                PushDspToEngine();
                NotifyTray();
            }
        }
    }

    private void PollTelemetry()
    {
        if (!_uiVisible) return;
        var t = _engine.GetTelemetry(includeSpectrum: true);

        RoutingMode = (WaRoutingMode)t.RoutingMode;
        EngineSampleRate = t.EngineSampleRate > 0 ? t.EngineSampleRate : 48000;
        CpuLoadPercent = t.CpuLoadPercent;
        InPeakL = t.InputPeakDbL;
        InPeakR = t.InputPeakDbR;
        OutPeakL = t.OutputPeakDbL;
        OutPeakR = t.OutputPeakDbR;
        LimiterGrDb = t.LimiterGrDb;
        LoudnessGainDb = t.LoudnessGainDb;

        if (t.SpectrumDb != null)
        {
            SpectrumUpdated?.Invoke(t.SpectrumDb);
        }

        // Update per-device drift/buffer telemetry on Multi-Device cards
        if (t.Outputs != null)
        {
            var map = new Dictionary<string, WaOutputTelemetry>(StringComparer.OrdinalIgnoreCase);
            for (int i = 0; i < Math.Min(t.ActiveOutputCount, t.Outputs.Length); i++)
            {
                var o = t.Outputs[i];
                if (!string.IsNullOrEmpty(o.DeviceId)) map[o.DeviceId] = o;
            }
            foreach (var d in PhysicalDevices)
            {
                if (map.TryGetValue(d.Id, out var ot))
                {
                    d.WorkerState = (WaOutputState)ot.State;
                    d.BufferFillMs = ot.BufferFillMs;
                    d.DriftPpm = ot.DriftPpm;
                    d.Underruns = ot.Underruns;
                }
                else
                {
                    d.WorkerState = WaOutputState.Idle;
                }
            }
        }
    }

    private void NotifyTray() => TrayRefreshRequested?.Invoke();
    public void SaveConfigSafe() { try { _store.SaveConfig(Config); } catch { } }

    // --- Bound Properties ---
    public string ActiveSection { get => _activeSection; set { _activeSection = value; OnPropertyChanged(); } }
    public string ActiveAutoProfileStatus { get => _activeAutoProfileStatus; set { _activeAutoProfileStatus = value; OnPropertyChanged(); } }
    public string StatusBanner { get => _statusBanner; set { _statusBanner = value; OnPropertyChanged(); } }

    public bool HasVirtualSink => VirtualDevices.Count > 0;
    public string PrimaryDeviceName => PhysicalDevices.FirstOrDefault(d => d.IsDefault)?.Name
                                    ?? PhysicalDevices.FirstOrDefault()?.Name
                                    ?? "No Output Device";

    public string ActiveOutputSummary
    {
        get
        {
            int count = PhysicalDevices.Count(d => d.IsMultiEnabled);
            return Config.MultiDeviceEnabled && count > 1
                ? $"Multi-Out ({count} Devices)"
                : PrimaryDeviceName;
        }
    }

    public WaRoutingMode RoutingMode
    {
        get => _routingMode;
        private set
        {
            if (_routingMode != value)
            {
                _routingMode = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(RoutingModeBadge));
                OnPropertyChanged(nameof(RoutingModeDetail));
            }
        }
    }

    public string RoutingModeBadge => RoutingMode switch
    {
        WaRoutingMode.VirtualSink => "Full System DSP (Virtual Sink)",
        WaRoutingMode.DirectLoopback => "Direct / Loopback Multi-Out",
        _ => "Engine Stopped"
    };

    public string RoutingModeDetail => RoutingMode switch
    {
        WaRoutingMode.VirtualSink => "Full system-wide EQ, Maximizer, Spatial & Multi-Device routing active.",
        WaRoutingMode.DirectLoopback => "Zero-driver mode: instant device switching & synced Multi-Device mirroring active. Install VB-CABLE in Settings for single-device system DSP.",
        _ => "Engine idle."
    };

    public bool GlobalBypass
    {
        get => Config.GlobalBypass;
        set
        {
            if (Config.GlobalBypass != value)
            {
                Config.GlobalBypass = value;
                _engine.SetBypass(value);
                OnPropertyChanged();
                OnPropertyChanged(nameof(PowerLabel));
                SaveConfigSafe();
                NotifyTray();
            }
        }
    }

    public string PowerLabel => GlobalBypass ? "BYPASSED" : "ACTIVE";

    public bool MultiDeviceEnabled
    {
        get => Config.MultiDeviceEnabled;
        set
        {
            if (Config.MultiDeviceEnabled != value)
            {
                Config.MultiDeviceEnabled = value;
                ApplyOutputsToEngine();
                OnPropertyChanged();
                OnPropertyChanged(nameof(ActiveOutputSummary));
                OnPropertyChanged(nameof(CanCalibrateNow));
                SaveConfigSafe();
                NotifyTray();
            }
        }
    }

    public bool HasMicrophone
    {
        get => _hasMicrophone;
        private set { if (_hasMicrophone != value) { _hasMicrophone = value; OnPropertyChanged(); OnPropertyChanged(nameof(MicStatusText)); OnPropertyChanged(nameof(CanCalibrateNow)); } }
    }

    public string MicrophoneName
    {
        get => _microphoneName;
        private set { if (_microphoneName != value) { _microphoneName = value; OnPropertyChanged(); OnPropertyChanged(nameof(MicStatusText)); } }
    }

    public string MicStatusText => HasMicrophone
        ? $"Mic Ready: {MicrophoneName}"
        : "No Active Microphone Detected — Calibration Disabled";

    public bool IsCalibrating
    {
        get => _isCalibrating;
        private set { if (_isCalibrating != value) { _isCalibrating = value; OnPropertyChanged(); OnPropertyChanged(nameof(CanCalibrateNow)); } }
    }

    public string CalibrationStatusText
    {
        get => _calibrationStatusText;
        private set { if (_calibrationStatusText != value) { _calibrationStatusText = value; OnPropertyChanged(); } }
    }

    public bool AutoCalibrateOnDeviceAdd
    {
        get => Config.AutoCalibrateOnDeviceAdd;
        set { if (Config.AutoCalibrateOnDeviceAdd != value) { Config.AutoCalibrateOnDeviceAdd = value; OnPropertyChanged(); SaveConfigSafe(); } }
    }

    public bool CalibrateVolumeLevel
    {
        get => Config.CalibrateVolumeLevel;
        set { if (Config.CalibrateVolumeLevel != value) { Config.CalibrateVolumeLevel = value; OnPropertyChanged(); SaveConfigSafe(); } }
    }

    public bool CanCalibrateNow => HasMicrophone && !IsCalibrating && PhysicalDevices.Count(d => d.IsMultiEnabled || d.IsDirectLoopbackSource) >= 2;

    public bool TestToneEnabled
    {
        get => _testToneEnabled;
        set
        {
            if (_testToneEnabled != value)
            {
                _testToneEnabled = value;
                _engine.SetTestTone(value, 440f, -18f);
                OnPropertyChanged();
            }
        }
    }

    // Telemetry properties
    public int EngineSampleRate { get => _engineSampleRate; private set { if (_engineSampleRate != value) { _engineSampleRate = value; OnPropertyChanged(); OnPropertyChanged(nameof(CpuText)); } } }
    public float CpuLoadPercent { get => _cpuLoadPercent; private set { if (Math.Abs(_cpuLoadPercent - value) > 0.05f) { _cpuLoadPercent = value; OnPropertyChanged(); OnPropertyChanged(nameof(CpuText)); } } }
    public string CpuText => $"DSP {CpuLoadPercent:0.0}% • {EngineSampleRate / 1000.0:0.#} kHz";
    public double InPeakL { get => _inPeakL; private set { if (Math.Abs(_inPeakL - value) > 0.15) { _inPeakL = value; OnPropertyChanged(); } } }
    public double InPeakR { get => _inPeakR; private set { if (Math.Abs(_inPeakR - value) > 0.15) { _inPeakR = value; OnPropertyChanged(); } } }
    public double OutPeakL { get => _outPeakL; private set { if (Math.Abs(_outPeakL - value) > 0.15) { _outPeakL = value; OnPropertyChanged(); } } }
    public double OutPeakR { get => _outPeakR; private set { if (Math.Abs(_outPeakR - value) > 0.15) { _outPeakR = value; OnPropertyChanged(); } } }
    public double LimiterGrDb { get => _limiterGrDb; private set { if (Math.Abs(_limiterGrDb - value) > 0.05) { _limiterGrDb = value; OnPropertyChanged(); OnPropertyChanged(nameof(LimiterGrText)); } } }
    public string LimiterGrText => $"{LimiterGrDb:0.0} dB";
    public double LoudnessGainDb { get => _loudnessGainDb; private set { if (Math.Abs(_loudnessGainDb - value) > 0.05) { _loudnessGainDb = value; OnPropertyChanged(); OnPropertyChanged(nameof(LoudnessGainText)); } } }
    public string LoudnessGainText => $"{LoudnessGainDb:+0.0;-0.0;0.0} dB";

    // Current Profile DSP Bindings
    public string ActiveProfileName => CurrentProfile.Name;
    public double PreAmpDb { get => CurrentProfile.PreAmpDb; set { CurrentProfile.PreAmpDb = (float)value; OnPropertyChanged(); PushDspToEngine(); } }

    public bool BassEnabled { get => CurrentProfile.BassEnabled; set { CurrentProfile.BassEnabled = value; OnPropertyChanged(); PushDspToEngine(); } }
    public double BassBoostDb { get => CurrentProfile.BassBoostDb; set { CurrentProfile.BassBoostDb = (float)value; if (value > 0.1) CurrentProfile.BassEnabled = true; OnPropertyChanged(); OnPropertyChanged(nameof(BassEnabled)); PushDspToEngine(); } }
    public double BassFreqHz { get => CurrentProfile.BassFreqHz; set { CurrentProfile.BassFreqHz = (float)value; OnPropertyChanged(); PushDspToEngine(); } }
    public double BassExciterPercent { get => CurrentProfile.BassExciterMix * 100.0; set { CurrentProfile.BassExciterMix = (float)(value / 100.0); if (value > 1) CurrentProfile.BassEnabled = true; OnPropertyChanged(); OnPropertyChanged(nameof(BassEnabled)); PushDspToEngine(); } }

    public bool TrebleEnabled { get => CurrentProfile.TrebleEnabled; set { CurrentProfile.TrebleEnabled = value; OnPropertyChanged(); PushDspToEngine(); } }
    public double TrebleBoostDb { get => CurrentProfile.TrebleBoostDb; set { CurrentProfile.TrebleBoostDb = (float)value; if (value > 0.1) CurrentProfile.TrebleEnabled = true; OnPropertyChanged(); OnPropertyChanged(nameof(TrebleEnabled)); PushDspToEngine(); } }
    public double TrebleFreqHz { get => CurrentProfile.TrebleFreqHz; set { CurrentProfile.TrebleFreqHz = (float)value; OnPropertyChanged(); PushDspToEngine(); } }
    public double ClarityBoostDb { get => CurrentProfile.ClarityBoostDb; set { CurrentProfile.ClarityBoostDb = (float)value; if (value > 0.1) CurrentProfile.TrebleEnabled = true; OnPropertyChanged(); OnPropertyChanged(nameof(TrebleEnabled)); PushDspToEngine(); } }

    public bool EqEnabled { get => CurrentProfile.Eq.Enabled; set { CurrentProfile.Eq.Enabled = value; OnPropertyChanged(); PushDspToEngine(); } }
    public int EqModeIndex
    {
        get => (int)CurrentProfile.Eq.Mode;
        set
        {
            CurrentProfile.Eq.Mode = (WaEqMode)Math.Clamp(value, 0, 2);
            OnPropertyChanged();
            OnPropertyChanged(nameof(IsEq10));
            OnPropertyChanged(nameof(IsEq31));
            OnPropertyChanged(nameof(IsEqParam));
            PushDspToEngine();
        }
    }
    public bool IsEq10 => CurrentProfile.Eq.Mode == WaEqMode.Eq10Band;
    public bool IsEq31 => CurrentProfile.Eq.Mode == WaEqMode.Eq31Band;
    public bool IsEqParam => CurrentProfile.Eq.Mode == WaEqMode.Parametric;

    public int SelectedParamBandIndex
    {
        get => _selectedParamBandIndex;
        set { _selectedParamBandIndex = Math.Clamp(value, 0, EngineNative.MaxParamBands - 1); OnPropertyChanged(); OnPropertyChanged(nameof(SelectedParamBand)); OnPropertyChanged(nameof(ParamBandEnabled)); OnPropertyChanged(nameof(ParamBandTypeIndex)); OnPropertyChanged(nameof(ParamBandFreqHz)); OnPropertyChanged(nameof(ParamBandGainDb)); OnPropertyChanged(nameof(ParamBandQ)); }
    }
    public ParametricBandModel SelectedParamBand => CurrentProfile.Eq.ParametricBands[SelectedParamBandIndex];
    public bool ParamBandEnabled { get => SelectedParamBand.Enabled; set { SelectedParamBand.Enabled = value; OnPropertyChanged(); PushDspToEngine(); } }
    public int ParamBandTypeIndex { get => (int)SelectedParamBand.FilterType; set { SelectedParamBand.FilterType = (WaFilterType)Math.Clamp(value, 0, 5); OnPropertyChanged(); PushDspToEngine(); } }
    public double ParamBandFreqHz { get => SelectedParamBand.FrequencyHz; set { SelectedParamBand.FrequencyHz = (float)Math.Clamp(value, 20, 20000); OnPropertyChanged(); PushDspToEngine(); } }
    public double ParamBandGainDb { get => SelectedParamBand.GainDb; set { SelectedParamBand.GainDb = (float)Math.Clamp(value, -18, 18); OnPropertyChanged(); PushDspToEngine(); } }
    public double ParamBandQ { get => SelectedParamBand.Q; set { SelectedParamBand.Q = (float)Math.Clamp(value, 0.3, 12); OnPropertyChanged(); PushDspToEngine(); } }

    public bool LoudnessEnabled { get => CurrentProfile.LoudnessEnabled; set { CurrentProfile.LoudnessEnabled = value; OnPropertyChanged(); PushDspToEngine(); } }
    public double LoudnessTargetLufs { get => CurrentProfile.LoudnessTargetLufs; set { CurrentProfile.LoudnessTargetLufs = (float)value; OnPropertyChanged(); PushDspToEngine(); } }
    public double LoudnessStrengthPercent { get => CurrentProfile.LoudnessStrength * 100.0; set { CurrentProfile.LoudnessStrength = (float)(value / 100.0); OnPropertyChanged(); PushDspToEngine(); } }
    public double LoudnessMaxGainDb { get => CurrentProfile.LoudnessMaxGainDb; set { CurrentProfile.LoudnessMaxGainDb = (float)value; OnPropertyChanged(); PushDspToEngine(); } }

    public bool WidenerEnabled { get => CurrentProfile.WidenerEnabled; set { CurrentProfile.WidenerEnabled = value; OnPropertyChanged(); PushDspToEngine(); } }
    public double StereoWidthPercent { get => CurrentProfile.StereoWidth * 100.0; set { CurrentProfile.StereoWidth = (float)(value / 100.0); OnPropertyChanged(); PushDspToEngine(); } }
    public double BassMonoFreqHz { get => CurrentProfile.BassMonoFreqHz; set { CurrentProfile.BassMonoFreqHz = (float)value; OnPropertyChanged(); PushDspToEngine(); } }

    public int SurroundModeIndex { get => (int)CurrentProfile.SurroundMode; set { CurrentProfile.SurroundMode = (WaSurroundMode)Math.Clamp(value, 0, 2); OnPropertyChanged(); PushDspToEngine(); } }
    public double SurroundDepthPercent { get => CurrentProfile.SurroundDepth * 100.0; set { CurrentProfile.SurroundDepth = (float)(value / 100.0); OnPropertyChanged(); PushDspToEngine(); } }

    public bool AmbienceEnabled { get => CurrentProfile.AmbienceEnabled; set { CurrentProfile.AmbienceEnabled = value; OnPropertyChanged(); PushDspToEngine(); } }
    public double AmbienceRoomPercent { get => CurrentProfile.AmbienceRoomSize * 100.0; set { CurrentProfile.AmbienceRoomSize = (float)(value / 100.0); OnPropertyChanged(); PushDspToEngine(); } }
    public double AmbienceDampPercent { get => CurrentProfile.AmbienceDamping * 100.0; set { CurrentProfile.AmbienceDamping = (float)(value / 100.0); OnPropertyChanged(); PushDspToEngine(); } }
    public double AmbienceMixPercent { get => CurrentProfile.AmbienceMix * 100.0; set { CurrentProfile.AmbienceMix = (float)(value / 100.0); OnPropertyChanged(); PushDspToEngine(); } }

    public bool MaximizerEnabled { get => CurrentProfile.MaximizerEnabled; set { CurrentProfile.MaximizerEnabled = value; OnPropertyChanged(); PushDspToEngine(); } }
    public double MaximizerBoostDb { get => CurrentProfile.MaximizerBoostDb; set { CurrentProfile.MaximizerBoostDb = (float)value; if (value > 0.1) CurrentProfile.MaximizerEnabled = true; OnPropertyChanged(); OnPropertyChanged(nameof(MaximizerEnabled)); PushDspToEngine(); } }
    public double MaximizerCeilingDb { get => CurrentProfile.MaximizerCeilingDb; set { CurrentProfile.MaximizerCeilingDb = (float)value; OnPropertyChanged(); PushDspToEngine(); } }
    public double MaximizerSoftClipPercent { get => CurrentProfile.MaximizerSoftClip * 100.0; set { CurrentProfile.MaximizerSoftClip = (float)(value / 100.0); OnPropertyChanged(); PushDspToEngine(); } }
    public double MaximizerReleaseMs { get => CurrentProfile.MaximizerReleaseMs; set { CurrentProfile.MaximizerReleaseMs = (float)value; OnPropertyChanged(); PushDspToEngine(); } }

    // Settings bindings
    public bool PreferVirtualSink { get => Config.PreferVirtualSink; set { Config.PreferVirtualSink = value; OnPropertyChanged(); SaveConfigSafe(); RestartEngine(); } }
    public int BufferLatencyMs { get => Config.BufferLatencyMs; set { Config.BufferLatencyMs = Math.Clamp(value, 10, 60); OnPropertyChanged(); SaveConfigSafe(); RestartEngine(); } }
    public bool AutoProfileSwitchingEnabled { get => Config.AutoProfileSwitchingEnabled; set { Config.AutoProfileSwitchingEnabled = value; OnPropertyChanged(); SaveConfigSafe(); } }
    public bool ManualProfileLock { get => Config.ManualProfileLock; set { Config.ManualProfileLock = value; OnPropertyChanged(); SaveConfigSafe(); } }
    public bool GlobalHotkeysMasterEnabled { get => Config.GlobalHotkeysMasterEnabled; set { Config.GlobalHotkeysMasterEnabled = value; OnPropertyChanged(); ApplyHotkeys(); } }
    public bool StartWithWindows { get => Config.StartWithWindows; set { Config.StartWithWindows = value; StartupService.SetAutoStart(value, Config.StartMinimizedToTray); OnPropertyChanged(); SaveConfigSafe(); } }
    public bool StartMinimizedToTray { get => Config.StartMinimizedToTray; set { Config.StartMinimizedToTray = value; if (Config.StartWithWindows) StartupService.SetAutoStart(true, value); OnPropertyChanged(); SaveConfigSafe(); } }
    public bool MinimizeToTrayOnClose { get => Config.MinimizeToTrayOnClose; set { Config.MinimizeToTrayOnClose = value; OnPropertyChanged(); SaveConfigSafe(); } }

    private void NotifyAllDspProperties()
    {
        OnPropertyChanged(string.Empty);
    }

    public void Dispose()
    {
        _telemetryTimer.Stop();
        _autoProfileTimer.Stop();
        SaveConfigSafe();
        _hotkeys.Dispose();
        _engine.Dispose();
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
