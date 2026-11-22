#pragma once

#include "WinAudioABI.h"
#include <functional>
#include <string>
#include <vector>

namespace winaudio::audio {

struct ChirpAnalysis {
    float latencyMs = 0.0f;
    float rmsDb = -90.0f;
    float confidence = 0.0f; // 0..1 normalized correlation SNR
    bool detected = false;
};

// Acoustic Microphone Auto-Sync Calibrator.
// Generates a band-limited logarithmic sine sweep (450 Hz -> 3800 Hz, 180 ms) and uses
// matched-filter cross-correlation on the system microphone capture to measure exact
// end-to-end acoustic latency (sub-millisecond precision) and relative loudness per speaker.
class AcousticCalibrator {
public:
    static constexpr double kChirpStartHz = 450.0;
    static constexpr double kChirpEndHz = 3800.0;
    static constexpr double kChirpDurationSec = 0.180;

    // Pure deterministic DSP functions (unit-tested offline in WinAudioEngineTests)
    static std::vector<float> generateLogChirp(double sampleRate, float amplitude = 0.35f);
    static ChirpAnalysis analyzeRecording(const std::vector<float>& micMono,
                                          double micSampleRate,
                                          double chirpStartOffsetSec,
                                          double maxExpectedLatencyMs = 650.0);

    // Computes synchronized SyncDelayMs (aligning all faster devices to the slowest device)
    // and optional acoustic loudness-matching volume trims.
    static void computeRelativeAdjustments(std::vector<WaCalibrationResult>& results);

    // Hardware microphone detection & live WASAPI mic capture
    static bool findActiveMicrophone(std::wstring& outMicId, std::wstring& outMicName);

    // Captures durationMs of mono audio from the specified active microphone while invoking
    // onPlaybackReady() at the exact timestamp playback should begin (returns chirpStartOffsetSec).
    static bool captureFromMicrophone(const std::wstring& micId,
                                      int totalCaptureMs,
                                      int preRollMs,
                                      std::vector<float>& outMono,
                                      double& outSampleRate,
                                      double& outTriggerOffsetSec,
                                      const std::function<void()>& triggerChirp);
};

} // namespace winaudio::audio
