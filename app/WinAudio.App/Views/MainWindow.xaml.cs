using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using WinAudio.App.Models;
using WinAudio.App.Services;
using WinAudio.App.ViewModels;

namespace WinAudio.App.Views;

public partial class MainWindow : Window
{
    private readonly MainViewModel _vm;
    private bool _syncingTopPreset;
    private HotkeyRowViewModel? _capturingHotkeyRow;

    public event Action? ExitRequested;

    public MainWindow(MainViewModel vm)
    {
        InitializeComponent();
        _vm = vm;
        DataContext = vm;

        Loaded += OnLoaded;
        PreviewKeyDown += OnWindowPreviewKeyDown;

        // Wire interactive EQ canvases (both Dashboard preview & full Equalizer workspace)
        _vm.RegisterCurveFrequencies(MainEqCanvas.CurveFrequencies);
        DashboardEqCanvas.BandChanged += _vm.HandleCanvasBandDrag;
        MainEqCanvas.BandChanged += _vm.HandleCanvasBandDrag;
        MainEqCanvas.BandSelected += idx => _vm.SelectedParamBandIndex = idx;

        _vm.EqVisualsUpdated += (profile, curve, spec) =>
        {
            Dispatcher.BeginInvoke(() =>
            {
                DashboardEqCanvas.UpdateVisuals(profile, curve, spec);
                MainEqCanvas.UpdateVisuals(profile, curve, spec);
            });
        };

        _vm.SpectrumUpdated += spec =>
        {
            DashboardEqCanvas.UpdateSpectrumOnly(spec);
            MainEqCanvas.UpdateSpectrumOnly(spec);
        };

        _vm.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(MainViewModel.ActiveProfileName) || string.IsNullOrEmpty(e.PropertyName))
                Dispatcher.BeginInvoke(SyncTopPreset);
        };
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        _vm.InitializeHotkeys(hwnd);
        SyncTopPreset();
        _vm.RefreshRunningApps();
        _vm.RequestCurveRedraw();
    }

    private void SyncTopPreset()
    {
        _syncingTopPreset = true;
        TopPresetCombo.SelectedItem = _vm.Presets.FirstOrDefault(p => p.Id == _vm.CurrentProfile.Id);
        _syncingTopPreset = false;
    }

    private void OnTopPresetSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncingTopPreset) return;
        if (TopPresetCombo.SelectedItem is AudioProfile p)
        {
            _vm.SelectPreset(p.Id, "Manual");
        }
    }

    private void OnNavChecked(object sender, RoutedEventArgs e)
    {
        if (PanelDashboard == null) return;
        if (sender is not System.Windows.Controls.RadioButton rb || rb.Tag is not string tag) return;

        PanelDashboard.Visibility = tag == "Dashboard" ? Visibility.Visible : Visibility.Collapsed;
        PanelEqualizer.Visibility = tag == "Equalizer" ? Visibility.Visible : Visibility.Collapsed;
        PanelSpatial.Visibility = tag == "Spatial" ? Visibility.Visible : Visibility.Collapsed;
        PanelOutputs.Visibility = tag == "Outputs" ? Visibility.Visible : Visibility.Collapsed;
        PanelProfiles.Visibility = tag == "Profiles" ? Visibility.Visible : Visibility.Collapsed;
        PanelSettings.Visibility = tag == "Settings" ? Visibility.Visible : Visibility.Collapsed;

        if (tag == "Profiles") _vm.RefreshRunningApps();
        if (tag is "Dashboard" or "Equalizer") _vm.RequestCurveRedraw();
    }

    private void OnGoEq(object sender, RoutedEventArgs e) => NavEq.IsChecked = true;
    private void OnGoOutputs(object sender, RoutedEventArgs e) => NavOutputs.IsChecked = true;
    private void OnGoSettings(object sender, RoutedEventArgs e) => NavSettings.IsChecked = true;

    private void OnQuickSwitchDeviceClick(object sender, RoutedEventArgs e)
    {
        if (sender is System.Windows.Controls.Button btn && btn.Tag is string devId)
            _vm.SwitchDefaultDevice(devId);
    }

    private void OnNudgeDelayClick(object sender, RoutedEventArgs e)
    {
        if (sender is System.Windows.Controls.Button btn &&
            btn.DataContext is DeviceViewModel dev &&
            btn.Tag is string deltaStr &&
            float.TryParse(deltaStr, out float delta))
        {
            dev.SyncDelayMs = Math.Clamp((float)Math.Round(dev.SyncDelayMs + delta), 0f, 500f);
        }
    }

    private void OnFlattenEqClick(object sender, RoutedEventArgs e) => _vm.FlattenEq();
    private void OnSyncPulseClick(object sender, RoutedEventArgs e) => _vm.TriggerSyncPulse();
    private async void OnRunMicCalibrationClick(object sender, RoutedEventArgs e) => await _vm.RunAutoCalibrationAsync();
    private void OnRefreshDevicesClick(object sender, RoutedEventArgs e) => _vm.RefreshDevices();
    private void OnOpenVbCableClick(object sender, RoutedEventArgs e) => StartupService.OpenVbCableDownloadPage();
    private void OnRestartEngineClick(object sender, RoutedEventArgs e) => _vm.RestartEngine();
    private void OnExitClick(object sender, RoutedEventArgs e) => ExitRequested?.Invoke();

    private void OnSavePresetClick(object sender, RoutedEventArgs e)
    {
        string name = NewPresetNameBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(name)) name = $"{_vm.CurrentProfile.Name} (Custom)";
        _vm.SaveCurrentAsUserPreset(name);
        NewPresetNameBox.Clear();
    }

    private void OnApplyPresetCardClick(object sender, RoutedEventArgs e)
    {
        if (sender is System.Windows.Controls.Button btn && btn.Tag is string id)
            _vm.SelectPreset(id, "Manual");
    }

    private void OnDeletePresetClick(object sender, RoutedEventArgs e)
    {
        if (sender is System.Windows.Controls.Button btn && btn.Tag is string id)
            _vm.DeleteUserPreset(id);
    }

    private void OnImportPresetClick(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Filter = "Presets & Equalizer APO (*.json;*.txt)|*.json;*.txt|All Files (*.*)|*.*",
            Title = "Import WinAudio Preset or Equalizer APO Filter File"
        };
        if (dlg.ShowDialog() == true)
        {
            _vm.ImportPreset(dlg.FileName);
        }
    }

    private void OnExportPresetClick(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.SaveFileDialog
        {
            Filter = "WinAudio Preset (*.json)|*.json",
            FileName = $"{_vm.CurrentProfile.Name}.json",
            Title = "Export Current Audio Preset"
        };
        if (dlg.ShowDialog() == true)
        {
            _vm.ExportCurrentPreset(dlg.FileName);
        }
    }

    private void OnAddAppRuleClick(object sender, RoutedEventArgs e)
    {
        string proc = AppProcessCombo.Text.Trim();
        if (string.IsNullOrWhiteSpace(proc)) return;
        string? profId = (AppPresetCombo.SelectedItem as AudioProfile)?.Id ?? _vm.CurrentProfile.Id;
        _vm.AddAppRule(proc, profId);
        AppProcessCombo.Text = "";
    }

    private void OnRemoveAppRuleClick(object sender, RoutedEventArgs e)
    {
        if (sender is System.Windows.Controls.Button btn && btn.Tag is AppProfileRule rule)
            _vm.RemoveAppRule(rule);
    }

    private void OnBindDeviceProfileClick(object sender, RoutedEventArgs e)
    {
        if (DevMapDeviceCombo.SelectedItem is DeviceViewModel dev && DevMapPresetCombo.SelectedItem is AudioProfile prof)
        {
            _vm.SetDeviceProfileMapping(dev.Id, dev.Name, prof.Id);
        }
    }

    private void OnRemoveDeviceMappingClick(object sender, RoutedEventArgs e)
    {
        if (sender is System.Windows.Controls.Button btn && btn.Tag is DeviceProfileMapping m)
            _vm.SetDeviceProfileMapping(m.DeviceId, m.DeviceName, null);
    }

    // Opt-in Global Hotkey interactive capture
    private void OnBindHotkeyClick(object sender, RoutedEventArgs e)
    {
        if (sender is System.Windows.Controls.Button btn && btn.Tag is HotkeyRowViewModel row)
        {
            _capturingHotkeyRow = row;
            row.StatusText = "Press modifier + key now (Esc to cancel)...";
        }
    }

    private void OnClearHotkeyClick(object sender, RoutedEventArgs e)
    {
        if (sender is System.Windows.Controls.Button btn && btn.Tag is HotkeyRowViewModel row)
        {
            row.Model.Modifiers = 0;
            row.Model.VirtualKey = 0;
            row.Model.Enabled = false;
            _vm.ApplyHotkeys();
        }
    }

    private void OnWindowPreviewKeyDown(object sender, System.Windows.Input.KeyEventArgs e)
    {
        if (_capturingHotkeyRow == null) return;
        e.Handled = true;

        Key key = (e.Key == Key.System) ? e.SystemKey : e.Key;
        if (key == Key.Escape)
        {
            _capturingHotkeyRow = null;
            _vm.ApplyHotkeys();
            return;
        }

        // Ignore lone modifier key presses
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt or Key.LeftShift or Key.RightShift or Key.LWin or Key.RWin)
            return;

        uint mods = GlobalHotkeyService.ConvertModifiers(Keyboard.Modifiers);
        if (mods == 0)
        {
            _capturingHotkeyRow.StatusText = "Please include Ctrl, Alt, Shift, or Win with the key!";
            return;
        }

        uint vk = (uint)KeyInterop.VirtualKeyFromKey(key);
        _capturingHotkeyRow.Model.Modifiers = mods;
        _capturingHotkeyRow.Model.VirtualKey = vk;
        _capturingHotkeyRow.Model.Enabled = true;
        _capturingHotkeyRow = null;

        if (!_vm.GlobalHotkeysMasterEnabled)
            _vm.GlobalHotkeysMasterEnabled = true;
        else
            _vm.ApplyHotkeys();
    }

    private void OnWindowClosing(object? sender, CancelEventArgs e)
    {
        if (_vm.MinimizeToTrayOnClose)
        {
            e.Cancel = true;
            Hide();
            _vm.SetUiVisibility(false);
        }
        else
        {
            ExitRequested?.Invoke();
        }
    }

    private void OnWindowStateChanged(object? sender, EventArgs e)
    {
        if (WindowState == WindowState.Minimized)
        {
            _vm.SetUiVisibility(false);
        }
        else if (IsVisible)
        {
            _vm.SetUiVisibility(true);
        }
    }
}
