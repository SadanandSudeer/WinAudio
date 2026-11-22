#pragma once

#include <string>

namespace winaudio::audio {

// Switches the Windows system default audio playback device via Windows IPolicyConfig COM interface
// (same standard mechanism used by SoundSwitch, EarTrumpet, Voicemeeter, FxSound).
bool setSystemDefaultEndpoint(const std::wstring& deviceId, bool includeCommunications = true) noexcept;

// Reads and writes native Windows Core Audio endpoint volume (0..1), mute, and L/R balance (-1..+1)
// via IAudioEndpointVolume — allows direct control of the Windows Default device in Direct Loopback mode.
bool getEndpointVolume(const std::wstring& deviceId, float& outVolume01, bool& outMuted, float& outBalance) noexcept;
bool setEndpointVolume(const std::wstring& deviceId, float volume01, bool muted, float balance) noexcept;

} // namespace winaudio::audio
