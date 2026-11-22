using System.Runtime.InteropServices;
using System.Windows.Input;
using System.Windows.Interop;
using WinAudio.App.Models;

namespace WinAudio.App.Services;

// Opt-in Win32 Global Hotkey Manager.
// Registers ZERO hotkeys by default so WinAudio never conflicts with IDEs, games, or overlays.
// When the user explicitly enables & binds a shortcut in Settings, reports live Windows conflict status.
public sealed class GlobalHotkeyService : IDisposable
{
    public const uint ModAlt = 0x0001;
    public const uint ModControl = 0x0002;
    public const uint ModShift = 0x0004;
    public const uint ModWin = 0x0008;
    public const uint ModNoRepeat = 0x4000;
    private const int WmHotkey = 0x0312;

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnregisterHotKey(IntPtr hWnd, int id);

    private HwndSource? _hwndSource;
    private readonly Dictionary<int, string> _idToAction = new();
    private int _nextId = 9001;

    public event Action<string>? HotkeyTriggered;

    public void Initialize(IntPtr windowHandle)
    {
        if (_hwndSource != null) return;
        _hwndSource = HwndSource.FromHwnd(windowHandle);
        _hwndSource?.AddHook(WndProc);
    }

    public Dictionary<string, bool> ApplyBindings(bool masterEnabled, IEnumerable<HotkeyBinding> bindings)
    {
        var status = new Dictionary<string, bool>();
        UnregisterAll();

        if (!masterEnabled || _hwndSource == null)
        {
            foreach (var b in bindings) status[b.ActionId] = true;
            return status;
        }

        foreach (var b in bindings)
        {
            if (!b.IsBound)
            {
                status[b.ActionId] = true;
                continue;
            }

            int id = _nextId++;
            bool ok = RegisterHotKey(_hwndSource.Handle, id, b.Modifiers | ModNoRepeat, b.VirtualKey);
            if (ok)
            {
                _idToAction[id] = b.ActionId;
            }
            status[b.ActionId] = ok;
        }

        return status;
    }

    public void UnregisterAll()
    {
        if (_hwndSource != null)
        {
            foreach (var id in _idToAction.Keys)
            {
                UnregisterHotKey(_hwndSource.Handle, id);
            }
        }
        _idToAction.Clear();
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WmHotkey)
        {
            int id = wParam.ToInt32();
            if (_idToAction.TryGetValue(id, out var actionId))
            {
                HotkeyTriggered?.Invoke(actionId);
                handled = true;
            }
        }
        return IntPtr.Zero;
    }

    public static string FormatBinding(HotkeyBinding binding)
    {
        if (!binding.IsBound) return "[Not Set]";
        var parts = new List<string>();
        if ((binding.Modifiers & ModControl) != 0) parts.Add("Ctrl");
        if ((binding.Modifiers & ModAlt) != 0) parts.Add("Alt");
        if ((binding.Modifiers & ModShift) != 0) parts.Add("Shift");
        if ((binding.Modifiers & ModWin) != 0) parts.Add("Win");
        var key = KeyInterop.KeyFromVirtualKey((int)binding.VirtualKey);
        parts.Add(key.ToString());
        return string.Join(" + ", parts);
    }

    public static uint ConvertModifiers(ModifierKeys mods)
    {
        uint m = 0;
        if (mods.HasFlag(ModifierKeys.Control)) m |= ModControl;
        if (mods.HasFlag(ModifierKeys.Alt)) m |= ModAlt;
        if (mods.HasFlag(ModifierKeys.Shift)) m |= ModShift;
        if (mods.HasFlag(ModifierKeys.Windows)) m |= ModWin;
        return m;
    }

    public void Dispose()
    {
        UnregisterAll();
        _hwndSource?.RemoveHook(WndProc);
        _hwndSource = null;
    }
}
