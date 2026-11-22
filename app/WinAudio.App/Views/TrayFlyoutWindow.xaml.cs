using System.Windows;
using System.Windows.Controls;
using WinAudio.App.Models;
using WinAudio.App.ViewModels;

namespace WinAudio.App.Views;

public partial class TrayFlyoutWindow : Window
{
    private readonly MainViewModel _vm;
    private bool _syncingPreset;

    public event Action? OpenMainRequested;
    public event Action? ExitRequested;

    public TrayFlyoutWindow(MainViewModel vm)
    {
        InitializeComponent();
        _vm = vm;
        DataContext = vm;
        SyncPresetSelection();
        _vm.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(MainViewModel.ActiveProfileName) || string.IsNullOrEmpty(e.PropertyName))
                Dispatcher.BeginInvoke(SyncPresetSelection);
        };
    }

    public void PositionNearTray()
    {
        UpdateLayout();
        var area = SystemParameters.WorkArea;
        Left = Math.Max(area.Left + 8, area.Right - Width - 10);
        Top = Math.Max(area.Top + 8, area.Bottom - ActualHeight - 10);
    }

    private void SyncPresetSelection()
    {
        _syncingPreset = true;
        FlyoutPresetBox.SelectedItem = _vm.Presets.FirstOrDefault(p => p.Id == _vm.CurrentProfile.Id);
        _syncingPreset = false;
    }

    private void OnPresetChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncingPreset) return;
        if (FlyoutPresetBox.SelectedItem is AudioProfile p)
        {
            _vm.SelectPreset(p.Id, "Tray Flyout");
        }
    }

    private void OnDeviceCardClick(object sender, RoutedEventArgs e)
    {
        if (sender is System.Windows.Controls.Button btn && btn.Tag is string devId)
        {
            _vm.SwitchDefaultDevice(devId);
        }
    }

    private void OnOpenMain(object sender, RoutedEventArgs e) => OpenMainRequested?.Invoke();
    private void OnExit(object sender, RoutedEventArgs e) => ExitRequested?.Invoke();
    private void OnDeactivated(object? sender, EventArgs e) => Hide();
}
