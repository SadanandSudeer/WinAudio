using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;
using WinAudio.App.Models;

namespace WinAudio.App.Services;

public sealed class TrayIconService : IDisposable
{
    private readonly NotifyIcon _notifyIcon;
    private readonly ContextMenuStrip _menu;
    private readonly ToolStripMenuItem _bypassItem;
    private readonly ToolStripMenuItem _multiItem;
    private readonly ToolStripMenuItem _devicesSubMenu;
    private readonly ToolStripMenuItem _presetsSubMenu;
    private Icon _activeIcon;
    private Icon _bypassIcon;

    public event Action? LeftClicked;
    public event Action? OpenRequested;
    public event Action? BypassToggled;
    public event Action? MultiDeviceToggled;
    public event Action<string>? DeviceSelected;
    public event Action<string>? PresetSelected;
    public event Action? ExitRequested;

    public TrayIconService()
    {
        _activeIcon = CreateAppIcon(Color.FromArgb(16, 185, 129), Color.FromArgb(56, 189, 248));
        _bypassIcon = CreateAppIcon(Color.FromArgb(100, 116, 139), Color.FromArgb(148, 163, 184));

        _menu = new ContextMenuStrip();
        _menu.Items.Add("Open WinAudio", null, (_, _) => OpenRequested?.Invoke());
        _menu.Items.Add(new ToolStripSeparator());

        _bypassItem = new ToolStripMenuItem("Bypass Audio Processing", null, (_, _) => BypassToggled?.Invoke());
        _multiItem = new ToolStripMenuItem("Multi-Device Output", null, (_, _) => MultiDeviceToggled?.Invoke());
        _devicesSubMenu = new ToolStripMenuItem("Switch Output Device");
        _presetsSubMenu = new ToolStripMenuItem("Switch Preset");

        _menu.Items.Add(_bypassItem);
        _menu.Items.Add(_multiItem);
        _menu.Items.Add(_devicesSubMenu);
        _menu.Items.Add(_presetsSubMenu);
        _menu.Items.Add(new ToolStripSeparator());
        _menu.Items.Add("Exit WinAudio (Restore Audio)", null, (_, _) => ExitRequested?.Invoke());

        _notifyIcon = new NotifyIcon
        {
            Icon = _activeIcon,
            Text = "WinAudio — Active",
            Visible = true,
            ContextMenuStrip = _menu
        };

        _notifyIcon.MouseClick += (_, e) =>
        {
            if (e.Button == MouseButtons.Left)
                LeftClicked?.Invoke();
        };
        _notifyIcon.DoubleClick += (_, _) => OpenRequested?.Invoke();
    }

    public void UpdateState(bool bypassed, bool multiEnabled, string activePresetName, IEnumerable<DeviceViewModel> devices, IEnumerable<AudioProfile> presets)
    {
        _notifyIcon.Icon = bypassed ? _bypassIcon : _activeIcon;
        string tip = $"WinAudio • {(bypassed ? "Bypassed" : activePresetName)}";
        if (tip.Length > 63) tip = tip[..63];
        _notifyIcon.Text = tip;

        _bypassItem.Checked = bypassed;
        _multiItem.Checked = multiEnabled;

        _devicesSubMenu.DropDownItems.Clear();
        foreach (var d in devices.Where(x => !x.IsVirtualSink))
        {
            var devId = d.Id;
            var item = new ToolStripMenuItem($"{d.Name} ({d.TransportBadge})", null, (_, _) => DeviceSelected?.Invoke(devId))
            {
                Checked = d.IsDefault
            };
            _devicesSubMenu.DropDownItems.Add(item);
        }

        _presetsSubMenu.DropDownItems.Clear();
        foreach (var p in presets)
        {
            var pid = p.Id;
            var item = new ToolStripMenuItem(p.Name, null, (_, _) => PresetSelected?.Invoke(pid))
            {
                Checked = p.Name == activePresetName
            };
            _presetsSubMenu.DropDownItems.Add(item);
        }
    }

    private static Icon CreateAppIcon(Color c1, Color c2)
    {
        using var bmp = new Bitmap(32, 32);
        using var g = Graphics.FromImage(bmp);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.Clear(Color.Transparent);

        using (var bg = new LinearGradientBrush(new Rectangle(0, 0, 32, 32), Color.FromArgb(20, 26, 38), Color.FromArgb(30, 41, 59), 45f))
        {
            g.FillEllipse(bg, 1, 1, 30, 30);
        }
        using (var pen = new Pen(c2, 1.8f))
        {
            g.DrawEllipse(pen, 1.5f, 1.5f, 29, 29);
        }

        // Stylized 3-bar equalizer glyph
        using var b1 = new SolidBrush(c1);
        using var b2 = new SolidBrush(c2);
        g.FillRectangle(b1, 8, 15, 4, 10);
        g.FillRectangle(b2, 14, 8, 4, 17);
        g.FillRectangle(b1, 20, 12, 4, 13);

        IntPtr hIcon = bmp.GetHicon();
        return Icon.FromHandle(hIcon);
    }

    public void Dispose()
    {
        _notifyIcon.Visible = false;
        _notifyIcon.Dispose();
        _menu.Dispose();
        _activeIcon.Dispose();
        _bypassIcon.Dispose();
    }
}
