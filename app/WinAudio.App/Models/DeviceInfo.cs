using System.ComponentModel;
using System.Runtime.CompilerServices;
using WinAudio.App.Interop;

namespace WinAudio.App.Models;

public sealed class DeviceViewModel : INotifyPropertyChanged
{
    private bool _isMultiEnabled;
    private float _volume = 1.0f;
    private bool _mute;
    private float _balance;
    private float _syncDelayMs;
    private string? _customEqProfileId;
    private WaOutputState _workerState = WaOutputState.Idle;
    private float _bufferFillMs;
    private float _driftPpm;
    private uint _underruns;
    private bool _isDirectLoopbackSource;

    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public WaDeviceTransport Transport { get; set; }
    public bool IsDefault { get; set; }
    public bool IsDefaultComm { get; set; }
    public bool IsVirtualSink { get; set; }
    public bool IsBluetoothHfp { get; set; }
    public int SampleRate { get; set; }
    public int Channels { get; set; }

    public string TransportBadge => Transport switch
    {
        WaDeviceTransport.BluetoothA2dp => "Bluetooth A2DP",
        WaDeviceTransport.BluetoothHfp => "Bluetooth HFP (Low BW)",
        WaDeviceTransport.Hdmi => "HDMI / DisplayPort",
        WaDeviceTransport.Usb => "USB Audio",
        WaDeviceTransport.Headphones => "Wired Headphones",
        WaDeviceTransport.WiredSpeakers => "Wired / Speakers",
        WaDeviceTransport.Virtual => "Virtual Sink",
        _ => "Audio Device"
    };

    public string TransportIcon => Transport switch
    {
        WaDeviceTransport.BluetoothA2dp => "\uE702",
        WaDeviceTransport.BluetoothHfp => "\uE702",
        WaDeviceTransport.Hdmi => "\uE7F4",
        WaDeviceTransport.Usb => "\uE88E",
        WaDeviceTransport.Headphones => "\uE7F6",
        WaDeviceTransport.Virtual => "\uE958",
        _ => "\uE7F5"
    };

    public string FormatText => $"{SampleRate / 1000.0:0.#} kHz • {Channels}ch";

    public bool IsMultiEnabled
    {
        get => _isMultiEnabled;
        set { if (_isMultiEnabled != value) { _isMultiEnabled = value; OnPropertyChanged(); } }
    }

    public float Volume
    {
        get => _volume;
        set { if (Math.Abs(_volume - value) > 0.001f) { _volume = value; OnPropertyChanged(); OnPropertyChanged(nameof(VolumePercentText)); } }
    }

    public string VolumePercentText => $"{Math.Round(Volume * 100)}%";

    public bool Mute
    {
        get => _mute;
        set { if (_mute != value) { _mute = value; OnPropertyChanged(); } }
    }

    public float Balance
    {
        get => _balance;
        set { if (Math.Abs(_balance - value) > 0.001f) { _balance = value; OnPropertyChanged(); } }
    }

    public float SyncDelayMs
    {
        get => _syncDelayMs;
        set { if (Math.Abs(_syncDelayMs - value) > 0.1f) { _syncDelayMs = value; OnPropertyChanged(); OnPropertyChanged(nameof(SyncDelayText)); } }
    }

    public string SyncDelayText => $"{Math.Round(SyncDelayMs)} ms";

    public string? CustomEqProfileId
    {
        get => _customEqProfileId;
        set { if (_customEqProfileId != value) { _customEqProfileId = value; OnPropertyChanged(); } }
    }

    public WaOutputState WorkerState
    {
        get => _workerState;
        set { if (_workerState != value) { _workerState = value; OnPropertyChanged(); OnPropertyChanged(nameof(HealthText)); } }
    }

    public float BufferFillMs
    {
        get => _bufferFillMs;
        set { if (Math.Abs(_bufferFillMs - value) > 0.2f) { _bufferFillMs = value; OnPropertyChanged(); OnPropertyChanged(nameof(HealthText)); } }
    }

    public float DriftPpm
    {
        get => _driftPpm;
        set { if (Math.Abs(_driftPpm - value) > 0.5f) { _driftPpm = value; OnPropertyChanged(); OnPropertyChanged(nameof(HealthText)); } }
    }

    public uint Underruns
    {
        get => _underruns;
        set { if (_underruns != value) { _underruns = value; OnPropertyChanged(); OnPropertyChanged(nameof(HealthText)); } }
    }

    public bool IsDirectLoopbackSource
    {
        get => _isDirectLoopbackSource;
        set
        {
            if (_isDirectLoopbackSource != value)
            {
                _isDirectLoopbackSource = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(MaxVolume));
                OnPropertyChanged(nameof(CanAdjustSyncDelay));
                OnPropertyChanged(nameof(HealthText));
            }
        }
    }

    public float MaxVolume => IsDirectLoopbackSource ? 1.0f : 1.5f;
    public bool CanAdjustSyncDelay => !IsDirectLoopbackSource;

    public void SetVolumeMuteBalanceSilent(float volume, bool mute, float balance)
    {
        _volume = Math.Clamp(volume, 0f, MaxVolume);
        _mute = mute;
        _balance = Math.Clamp(balance, -1f, 1f);
        OnPropertyChanged(nameof(Volume));
        OnPropertyChanged(nameof(VolumePercentText));
        OnPropertyChanged(nameof(Mute));
        OnPropertyChanged(nameof(Balance));
    }

    public string HealthText => IsDirectLoopbackSource
        ? "Loopback Source • Windows Direct Volume/Mute"
        : WorkerState switch
        {
            WaOutputState.Running => Underruns > 0
                ? $"Synced • Buf {BufferFillMs:0.0} ms • Drift {DriftPpm:+0;-0;0} ppm • Underruns {Underruns}"
                : $"Synced • Buf {BufferFillMs:0.0} ms • Drift {DriftPpm:+0;-0;0} ppm",
            WaOutputState.Reconnecting => "Reconnecting...",
            WaOutputState.Error => "Device Error",
            _ => "Standby"
        };

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
