using System.Threading;
using System.Windows;
using WinAudio.App.Services;
using WinAudio.App.ViewModels;
using WinAudio.App.Views;

namespace WinAudio.App;

public partial class App : System.Windows.Application
{
    private Mutex? _singleInstanceMutex;
    private AudioControllerService? _engine;
    private GlobalHotkeyService? _hotkeys;
    private TrayIconService? _tray;
    private MainViewModel? _viewModel;
    private MainWindow? _mainWindow;
    private TrayFlyoutWindow? _flyout;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        _singleInstanceMutex = new Mutex(true, "Global\\WinAudio_SingleInstance_Mutex", out bool createdNew);
        if (!createdNew)
        {
            System.Windows.MessageBox.Show("WinAudio is already running in the system tray.", "WinAudio", MessageBoxButton.OK, MessageBoxImage.Information);
            Shutdown();
            return;
        }

        // Crash safety: ensure Windows default playback device is restored if an unhandled exception occurs
        AppDomain.CurrentDomain.UnhandledException += (_, _) => SafeCleanup();
        DispatcherUnhandledException += (_, args) =>
        {
            SafeCleanup();
            args.Handled = false;
        };

        var store = new PresetStore();
        _engine = new AudioControllerService();
        _hotkeys = new GlobalHotkeyService();
        _viewModel = new MainViewModel(_engine, store, _hotkeys);

        _mainWindow = new MainWindow(_viewModel);
        this.MainWindow = _mainWindow;
        _tray = new TrayIconService();

        _viewModel.TrayRefreshRequested += () =>
        {
            Dispatcher.BeginInvoke(() =>
            {
                _tray?.UpdateState(
                    _viewModel.GlobalBypass,
                    _viewModel.MultiDeviceEnabled,
                    _viewModel.ActiveProfileName,
                    _viewModel.Devices,
                    _viewModel.Presets);
            });
        };

        _tray.OpenRequested += ShowMainWindow;
        _tray.LeftClicked += ToggleFlyout;
        _tray.BypassToggled += () => Dispatcher.Invoke(() => _viewModel.GlobalBypass = !_viewModel.GlobalBypass);
        _tray.MultiDeviceToggled += () => Dispatcher.Invoke(() => _viewModel.MultiDeviceEnabled = !_viewModel.MultiDeviceEnabled);
        _tray.DeviceSelected += devId => Dispatcher.Invoke(() => _viewModel.SwitchDefaultDevice(devId));
        _tray.PresetSelected += presetId => Dispatcher.Invoke(() => _viewModel.SelectPreset(presetId));
        _tray.ExitRequested += ExitApplication;

        _mainWindow.ExitRequested += ExitApplication;

        _tray.UpdateState(
            _viewModel.GlobalBypass,
            _viewModel.MultiDeviceEnabled,
            _viewModel.ActiveProfileName,
            _viewModel.Devices,
            _viewModel.Presets);

        bool startMinimized = e.Args.Any(a => a.Equals("--minimized", StringComparison.OrdinalIgnoreCase))
                           || _viewModel.Config.StartMinimizedToTray;

        if (startMinimized)
        {
            _mainWindow.Show();
            _mainWindow.Hide();
            _viewModel.SetUiVisibility(false);
        }
        else
        {
            _mainWindow.Show();
        }
    }

    private void ShowMainWindow()
    {
        if (_mainWindow == null || _viewModel == null) return;
        _flyout?.Hide();
        _mainWindow.Show();
        _mainWindow.WindowState = WindowState.Normal;
        _mainWindow.Activate();
        _viewModel.SetUiVisibility(true);
    }

    private void ToggleFlyout()
    {
        if (_viewModel == null) return;
        if (_flyout == null)
        {
            _flyout = new TrayFlyoutWindow(_viewModel);
            _flyout.OpenMainRequested += ShowMainWindow;
            _flyout.ExitRequested += ExitApplication;
        }

        if (_flyout.IsVisible)
        {
            _flyout.Hide();
        }
        else
        {
            _flyout.PositionNearTray();
            _flyout.Show();
            _flyout.Activate();
        }
    }

    private void ExitApplication()
    {
        SafeCleanup();
        Shutdown();
    }

    private void SafeCleanup()
    {
        try { _flyout?.Close(); } catch { }
        try { _tray?.Dispose(); } catch { }
        try { _viewModel?.Dispose(); } catch { }
        try { _singleInstanceMutex?.ReleaseMutex(); _singleInstanceMutex?.Dispose(); } catch { }
        _tray = null;
        _viewModel = null;
        _engine = null;
        _hotkeys = null;
    }
}
