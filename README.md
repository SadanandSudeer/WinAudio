# WinAudio — Comprehensive Windows 11 Sound Controller

**WinAudio** is a lightweight, gaming-safe Windows 11 audio control suite built with a native **C++20 real-time DSP & WASAPI multi-device engine** (`WinAudioEngine.dll`) and a **C# .NET 9 WPF Modern Fluent Dark UI** (`WinAudio.exe`).

## Features

- **Multi-Band Equalizer**: Switchable **10-band ISO**, **31-band 1/3-octave**, and **12-band Semi-Parametric** EQ (Peaking, Low/High Shelf, Low/High Pass, Notch) with an interactive draggable curve and live 2048-point FFT spectrum overlay.
- **Controlled Sound Booster / Maximizer**: Up to `+18 dB` boost protected by an O(1) monotonic-deque **3 ms lookahead brickwall limiter**, optional soft-clip warmth knee, `-0.3 dBFS` ceiling, and live Gain Reduction meter.
- **Bass & Treble Boosters**: Tunable bass low-shelf + **psychoacoustic sub-harmonic exciter** (adds perceived punch on small laptop/desktop speakers), plus treble air shelf and vocal clarity enhancer.
- **Spatial & Dynamic Effects**:
  - **Virtual Surround** (binaural headphone ITD/head-shadow + speaker soundstage modes)
  - **Mid/Side Stereo Widener** with Linkwitz-Riley bass-mono crossover
  - **Room Ambience** (4-channel Hadamard Feedback Delay Network)
  - **Loudness Normalization** (EBU R128 K-weighted RMS AGC with `-50 LUFS` silence gate)
- **Output Device Switching & Multi-Device Output**:
  - Instant Windows default playback switching (`IPolicyConfig`) + direct `IAudioEndpointVolume` control
  - Simultaneous playback across **multiple Bluetooth A2DP devices** and **Wired (3.5 mm, HDMI, USB) + Bluetooth**
  - **4-point Cubic Hermite ASRC + PI clock-drift controller** (handles `44.1 / 48 / 192 kHz` mismatches and crystal drift with zero glitches)
  - **Adaptive Bluetooth buffering & self-healing jitter escalation** for multi-Bluetooth stability
  - **Per-device Sync Delay (`0–500 ms` with `1 ms` / `5 ms` nudge buttons)** + **Sync Pulse Test**
- **Acoustic Microphone Auto-Calibration**:
  - Automatically detects if a physical microphone is attached (preferring wired/USB room mics so Bluetooth headsets stay in Stereo A2DP) and stays disabled if no mic is present
  - Uses matched-filter log-swept chirp (`450–3800 Hz`) cross-correlation to measure acoustic delay (`< 1 ms` accuracy) and optionally level speaker volumes whenever an additional device is enabled or on demand
- **Presets, Auto-Profiles & Quick Controls**:
  - 10 built-in presets, custom user presets, JSON import/export, and **Equalizer APO / AutoEQ `.txt` import**
  - Automatic **per-device** and **per-app** profile switching
  - **Opt-in global hotkeys** (all unbound by default to avoid conflicts), system tray left-click quick flyout, and click-free 5 ms bypass
- **Low Resource Usage & Gaming Safety**:
  - Zero heap allocations on audio threads, MMCSS `"Pro Audio"` scheduling, `0.00%` idle UI CPU when minimized to tray
  - **Zero Test Signing Mode** (100% anti-cheat safe): supports **WHQL-signed VB-CABLE** for Full System DSP with automatic **Zero-Driver WASAPI Loopback** fallback

## Architecture

```text
WinAudio/
├── CMakeLists.txt          # C++20 MSVC/Ninja build for WinAudioDSP, WinAudioEngine.dll & tests
├── build.ps1               # One-command Release build, test & portable packaging script
├── engine/
│   ├── include/            # Pure C-ABI contract (WinAudioABI.h)
│   ├── src/dsp/            # Equalizer, ToneBooster, LoudnessNorm, SpatialEffects, Maximizer, FFT
│   ├── src/audio/          # RingBuffer, DriftResampler, PolicyConfig, DeviceManager, Output/Capture Workers, AcousticCalibrator
│   └── tests/              # Native C++20 DSP, ASRC, Calibration & WASAPI test suite
└── app/
    ├── WinAudio.App/       # C# .NET 9 WPF Fluent Dark UI, tray flyout, auto-profiles, opt-in hotkeys
    └── WinAudio.Tests/     # C# ABI layout, preset, Equalizer APO import & auto-profile tests
```

## Build & Run

### Prerequisites
- Windows 11 (x64)
- Visual Studio 2022 (Desktop development with C++) + CMake + Ninja
- .NET 9 SDK

### One-Command Build, Test & Portable Package
```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```
This builds `WinAudioEngine.dll` (`Release`), runs all **54 automated tests** (33 C++20 + 21 C# .NET 9), and packages the portable application to `dist\WinAudio\WinAudio.exe`.

### Optional: Full System-Wide DSP Setup
For full single-device + multi-device system DSP without enabling Windows Test Signing Mode, install the free WHQL-signed **VB-CABLE** driver from [vb-audio.com/Cable](https://vb-audio.com/Cable/) and click **Restart Engine** in WinAudio Settings. Without VB-CABLE, WinAudio works immediately in Zero-Driver Direct/Loopback mode.
