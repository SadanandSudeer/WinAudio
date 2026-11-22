using System.Diagnostics;
using System.Runtime.InteropServices;
using WinAudio.App.Models;

namespace WinAudio.App.Services;

public sealed record ProfileResolutionResult(string ProfileId, string Reason);

// Monitors the foreground application & active output device to automatically apply
// user-defined Per-App and Per-Device profiles with deterministic priority:
//   Manual Lock > Per-App Rule (foreground or running matched app) > Per-Device Mapping > Fallback
public sealed class AutoProfileService
{
    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    public static string? GetForegroundProcessName()
    {
        try
        {
            var hwnd = GetForegroundWindow();
            if (hwnd == IntPtr.Zero) return null;
            GetWindowThreadProcessId(hwnd, out uint pid);
            if (pid == 0) return null;
            using var proc = Process.GetProcessById((int)pid);
            return proc.ProcessName;
        }
        catch
        {
            return null;
        }
    }

    public static List<string> GetRunningAppNames()
    {
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            foreach (var p in Process.GetProcesses())
            {
                try
                {
                    if (p.MainWindowHandle != IntPtr.Zero && !string.IsNullOrWhiteSpace(p.ProcessName))
                    {
                        names.Add(p.ProcessName);
                    }
                }
                catch { }
                finally { p.Dispose(); }
            }
        }
        catch { }
        return names.OrderBy(n => n, StringComparer.OrdinalIgnoreCase).ToList();
    }

    // Pure deterministic resolution function (unit-tested in WinAudio.Tests!)
    public static ProfileResolutionResult ResolveTargetProfile(
        AppConfig config,
        string? foregroundProcessName,
        IReadOnlyCollection<string> runningProcesses,
        string? activeDeviceId,
        string fallbackProfileId)
    {
        if (!config.AutoProfileSwitchingEnabled || config.ManualProfileLock)
        {
            return new ProfileResolutionResult(fallbackProfileId, "Manual");
        }

        // 1. Foreground app rule takes highest automatic priority
        if (!string.IsNullOrWhiteSpace(foregroundProcessName))
        {
            var fgRule = config.AppRules.FirstOrDefault(r =>
                r.Enabled &&
                !string.IsNullOrWhiteSpace(r.ProcessName) &&
                !string.IsNullOrWhiteSpace(r.ProfileId) &&
                NormalizeProc(r.ProcessName).Equals(NormalizeProc(foregroundProcessName), StringComparison.OrdinalIgnoreCase));

            if (fgRule != null)
            {
                return new ProfileResolutionResult(fgRule.ProfileId, $"App: {fgRule.ProcessName}");
            }
        }

        // 2. Background running app rule (e.g. Spotify playing while browsing)
        foreach (var rule in config.AppRules)
        {
            if (!rule.Enabled || string.IsNullOrWhiteSpace(rule.ProcessName) || string.IsNullOrWhiteSpace(rule.ProfileId))
                continue;
            string target = NormalizeProc(rule.ProcessName);
            if (runningProcesses.Any(p => NormalizeProc(p).Equals(target, StringComparison.OrdinalIgnoreCase)))
            {
                return new ProfileResolutionResult(rule.ProfileId, $"App (Active): {rule.ProcessName}");
            }
        }

        // 3. Per-Device profile mapping
        if (!string.IsNullOrWhiteSpace(activeDeviceId))
        {
            var devMap = config.DeviceMappings.FirstOrDefault(m =>
                !string.IsNullOrWhiteSpace(m.ProfileId) &&
                m.DeviceId.Equals(activeDeviceId, StringComparison.OrdinalIgnoreCase));

            if (devMap != null)
            {
                return new ProfileResolutionResult(devMap.ProfileId, $"Device: {devMap.DeviceName}");
            }
        }

        return new ProfileResolutionResult(fallbackProfileId, "Default");
    }

    private static string NormalizeProc(string name)
    {
        var trimmed = name.Trim();
        return trimmed.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)
            ? trimmed[..^4]
            : trimmed;
    }
}
