using System.Diagnostics;
using Microsoft.Win32;

namespace WinAudio.App.Services;

public static class StartupService
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string AppName = "WinAudio";

    public static bool IsAutoStartEnabled()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, false);
            return key?.GetValue(AppName) is string s && !string.IsNullOrWhiteSpace(s);
        }
        catch { return false; }
    }

    public static void SetAutoStart(bool enabled, bool startMinimized)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, true);
            if (key == null) return;
            if (enabled)
            {
                string? exePath = Environment.ProcessPath;
                if (!string.IsNullOrWhiteSpace(exePath))
                {
                    string cmd = startMinimized ? $"\"{exePath}\" --minimized" : $"\"{exePath}\"";
                    key.SetValue(AppName, cmd);
                }
            }
            else
            {
                key.DeleteValue(AppName, false);
            }
        }
        catch { }
    }

    public static void OpenVbCableDownloadPage()
    {
        try
        {
            Process.Start(new ProcessStartInfo("https://vb-audio.com/Cable/") { UseShellExecute = true });
        }
        catch { }
    }
}
