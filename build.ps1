$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root "build"
$distDir = Join-Path $root "dist\WinAudio"

Write-Host "=== 1. Building Native C++20 WinAudioEngine.dll (Release) ===" -ForegroundColor Cyan
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    $vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
}
cmd /c "`"$vcvars`" > nul && cmake -S `"$root`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build `"$buildDir`""
if ($LASTEXITCODE -ne 0) { throw "Native C++ build failed." }

Write-Host "`n=== 2. Running C++20 Engine & DSP Automated Tests ===" -ForegroundColor Cyan
& "$buildDir\WinAudioEngineTests.exe"
if ($LASTEXITCODE -ne 0) { throw "Native C++ tests failed." }

Write-Host "`n=== 3. Building C# .NET 9 WPF App & Test Suite (Release) ===" -ForegroundColor Cyan
dotnet build "$root\app\WinAudio.sln" -c Release
if ($LASTEXITCODE -ne 0) { throw ".NET build failed." }

Write-Host "`n=== 4. Running C# ABI, Preset & Auto-Profile Automated Tests ===" -ForegroundColor Cyan
dotnet run --project "$root\app\WinAudio.Tests\WinAudio.Tests.csproj" -c Release --no-build
if ($LASTEXITCODE -ne 0) { throw ".NET tests failed." }

Write-Host "`n=== 5. Packaging Portable Bundle -> dist\WinAudio ===" -ForegroundColor Cyan
if (Test-Path $distDir) { Remove-Item -Recurse -Force $distDir }
dotnet publish "$root\app\WinAudio.App\WinAudio.App.csproj" -c Release -r win-x64 --self-contained false -o "$distDir"
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed." }

Copy-Item "$buildDir\WinAudioEngine.dll" "$distDir\WinAudioEngine.dll" -Force

@"
WinAudio v1.0.0 — Portable Windows 11 Sound Controller
======================================================

Quick Start:
  1. Run WinAudio.exe (portable — all settings & presets save in .\Data\).
  2. Click the system tray icon for the Quick Control Flyout (Bypass, Device Switcher,
     Multi-Device toggle, Preset picker, Sound Maximizer).
  3. For Full System-Wide DSP on single & multiple outputs without Test Signing Mode
     (100% anti-cheat safe), install the free WHQL-signed VB-CABLE driver from
     https://vb-audio.com/Cable/ and click 'Restart Engine' in Settings.
  4. In Outputs & Multi-Device, enable any combination of Wired (3.5mm, HDMI, USB) +
     multiple Bluetooth A2DP devices simultaneously. Use the Sync Delay slider (0-500 ms)
     and 'Sync Pulse Test' to align faster wired outputs with Bluetooth latency.
  5. Global hotkeys are opt-in and all unbound by default so WinAudio never conflicts
     with other tools. Enable & bind any shortcut in Settings -> Global Keyboard Shortcuts.
"@ | Set-Content (Join-Path $distDir "README.txt") -Encoding UTF8

Write-Host "`n=== BUILD & PACKAGING COMPLETE ===" -ForegroundColor Green
Write-Host "Portable App: $distDir\WinAudio.exe"
