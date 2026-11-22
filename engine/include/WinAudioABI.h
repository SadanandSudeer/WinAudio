#pragma once

#include <stdint.h>

#ifdef _WIN32
#ifdef WINAUDIO_BUILD_DLL
#define WA_API __declspec(dllexport)
#else
#define WA_API __declspec(dllimport)
#endif
#else
#define WA_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WA_ABI_VERSION          1
#define WA_MAX_GRAPHIC_BANDS    31
#define WA_MAX_PARAM_BANDS      12
#define WA_MAX_OUTPUT_SLOTS     8
#define WA_MAX_DEVICES          64
#define WA_SPECTRUM_BINS        64
#define WA_DEVICE_ID_LEN        128
#define WA_DEVICE_NAME_LEN      160

// EQ Modes
typedef enum WaEqMode : int32_t {
    WA_EQ_10BAND = 0,
    WA_EQ_31BAND = 1,
    WA_EQ_PARAMETRIC = 2
} WaEqMode;

// Biquad Filter Types for Semi-Parametric EQ
typedef enum WaFilterType : int32_t {
    WA_FILTER_PEAKING = 0,
    WA_FILTER_LOW_SHELF = 1,
    WA_FILTER_HIGH_SHELF = 2,
    WA_FILTER_LOW_PASS = 3,
    WA_FILTER_HIGH_PASS = 4,
    WA_FILTER_NOTCH = 5
} WaFilterType;

// Virtual Surround Modes
typedef enum WaSurroundMode : int32_t {
    WA_SURROUND_OFF = 0,
    WA_SURROUND_HEADPHONES = 1,
    WA_SURROUND_SPEAKERS = 2
} WaSurroundMode;

// Device Transport / Type Classification
typedef enum WaDeviceTransport : int32_t {
    WA_TRANSPORT_UNKNOWN = 0,
    WA_TRANSPORT_WIRED_SPEAKERS = 1,
    WA_TRANSPORT_HEADPHONES = 2,
    WA_TRANSPORT_HDMI = 3,
    WA_TRANSPORT_USB = 4,
    WA_TRANSPORT_BLUETOOTH_A2DP = 5,
    WA_TRANSPORT_BLUETOOTH_HFP = 6,
    WA_TRANSPORT_VIRTUAL = 7
} WaDeviceTransport;

// Engine Routing Mode
typedef enum WaRoutingMode : int32_t {
    WA_ROUTING_STOPPED = 0,
    WA_ROUTING_VIRTUAL_SINK = 1,      // Full System DSP via Virtual Sink (e.g. VB-CABLE)
    WA_ROUTING_DIRECT_LOOPBACK = 2    // Zero-Driver Fallback via WASAPI Loopback
} WaRoutingMode;

// Per-Output Worker State
typedef enum WaOutputState : int32_t {
    WA_OUTPUT_IDLE = 0,
    WA_OUTPUT_RUNNING = 1,
    WA_OUTPUT_RECONNECTING = 2,
    WA_OUTPUT_ERROR = 3
} WaOutputState;

#pragma pack(push, 8)

typedef struct WaParamBand {
    int32_t enabled;       // 0 or 1
    int32_t filterType;    // WaFilterType
    float frequencyHz;     // 20.0 .. 20000.0
    float gainDb;          // -18.0 .. +18.0
    float q;               // 0.3 .. 12.0
    int32_t reserved;
} WaParamBand;

typedef struct WaEqParams {
    int32_t enabled;                             // 0 or 1
    int32_t mode;                                // WaEqMode
    float graphicGainsDb[WA_MAX_GRAPHIC_BANDS];  // 10-band uses [0..9], 31-band uses [0..30]
    int32_t reserved;
    WaParamBand paramBands[WA_MAX_PARAM_BANDS];
} WaEqParams;

typedef struct WaDspParams {
    int32_t globalBypass;       // 1 = click-free bypass active
    float preAmpDb;             // -18.0 .. +24.0

    // Bass & Treble Boosters
    int32_t bassEnabled;
    float bassBoostDb;          // 0.0 .. +15.0
    float bassFreqHz;           // 60.0 .. 150.0 (default 90)
    float bassExciterMix;       // 0.0 .. 1.0 (psychoacoustic sub-harmonic exciter for small speakers)

    int32_t trebleEnabled;
    float trebleBoostDb;        // 0.0 .. +15.0
    float trebleFreqHz;         // 3500.0 .. 10000.0 (default 6000)
    float clarityBoostDb;       // 0.0 .. +8.0 (vocal/detail presence around 3 kHz)

    // Multi-Band Equalizer
    WaEqParams eq;

    // Loudness Normalization (Gated EBU R128 K-Weighted AGC)
    int32_t loudnessEnabled;
    float loudnessTargetLufs;   // -20.0 .. -10.0 (default -15.0)
    float loudnessStrength;     // 0.0 .. 1.0 (default 0.65)
    float loudnessMaxGainDb;    // 0.0 .. +12.0 (default 9.0)

    // Spatial Effects
    int32_t widenerEnabled;
    float stereoWidth;          // 0.0 (mono) .. 2.0 (ultra-wide), 1.0 = normal
    float bassMonoFreqHz;       // 80.0 .. 300.0 (default 150.0)

    int32_t surroundMode;       // WaSurroundMode
    float surroundDepth;        // 0.0 .. 1.0

    int32_t ambienceEnabled;
    float ambienceRoomSize;     // 0.1 .. 1.0
    float ambienceDamping;      // 0.0 .. 1.0
    float ambienceMix;          // 0.0 .. 0.65

    // Controlled Sound Booster / Maximizer (3ms Lookahead Brickwall Limiter)
    int32_t maximizerEnabled;
    float maximizerBoostDb;     // 0.0 .. +18.0
    float maximizerCeilingDb;   // -3.0 .. -0.1 (default -0.3)
    float maximizerSoftClip;    // 0.0 .. 1.0 (soft saturation knee blend)
    float maximizerReleaseMs;   // 20.0 .. 300.0 (default 80.0)
} WaDspParams;

typedef struct WaDeviceInfo {
    wchar_t id[WA_DEVICE_ID_LEN];
    wchar_t name[WA_DEVICE_NAME_LEN];
    int32_t transport;          // WaDeviceTransport
    int32_t isDefault;          // 1 if Windows default multimedia render device
    int32_t isDefaultComm;      // 1 if Windows default communications render device
    int32_t isVirtualSink;      // 1 if detected as VB-CABLE / virtual audio sink
    int32_t isBluetoothHfp;     // 1 if Bluetooth Hands-Free (low-bandwidth warning)
    int32_t sampleRate;         // Native mix sample rate (e.g., 44100 or 48000)
    int32_t channels;           // Native channel count
    int32_t isActive;           // 1 if DEVICE_STATE_ACTIVE
} WaDeviceInfo;

typedef struct WaOutputSlotConfig {
    wchar_t deviceId[WA_DEVICE_ID_LEN];
    int32_t enabled;            // 1 = active in output fan-out
    float volume;               // 0.0 .. 1.5 (1.0 = unity)
    int32_t mute;               // 1 = muted
    float balance;              // -1.0 (L) .. +1.0 (R)
    float syncDelayMs;          // 0.0 .. 500.0 ms (for Wired + Bluetooth alignment)
    int32_t useCustomEq;        // 1 = apply per-device EQ override instead of flat post-master
    WaEqParams customEq;        // Optional per-device EQ trim
} WaOutputSlotConfig;

typedef struct WaOutputTelemetry {
    wchar_t deviceId[WA_DEVICE_ID_LEN];
    int32_t state;              // WaOutputState
    int32_t sampleRate;         // Endpoint mix sample rate
    float bufferFillMs;         // Current SPSC ring buffer fill level in ms
    float driftPpm;             // PI controller clock-drift adjustment in ppm
    float appliedDelayMs;       // Active sync delay in ms
    uint32_t underruns;         // Total underrun count since start
    uint32_t overruns;          // Total overrun count since start
    int32_t reserved;
} WaOutputTelemetry;

typedef struct WaTelemetry {
    int32_t routingMode;        // WaRoutingMode
    int32_t isRunning;          // 1 if capture/render engine is active
    int32_t isBypassed;         // 1 if master DSP is bypassed
    int32_t engineSampleRate;   // Internal DSP sample rate (e.g., 48000)

    float inputPeakDbL;
    float inputPeakDbR;
    float inputRmsDbL;
    float inputRmsDbR;

    float outputPeakDbL;
    float outputPeakDbR;
    float outputRmsDbL;
    float outputRmsDbR;

    float limiterGrDb;          // Negative dB of current gain reduction (e.g., -4.2)
    float loudnessGainDb;       // Current AGC gain applied in dB
    float cpuLoadPercent;       // Estimated DSP CPU usage %

    int32_t activeOutputCount;
    float spectrumDb[WA_SPECTRUM_BINS]; // 64 log-spaced FFT magnitudes (-90 .. 0 dB)
    WaOutputTelemetry outputs[WA_MAX_OUTPUT_SLOTS];
} WaTelemetry;

typedef struct WaCalibrationResult {
    wchar_t deviceId[WA_DEVICE_ID_LEN];
    float measuredLatencyMs;     // End-to-end acoustic delay measured via mic (ms)
    float recommendedDelayMs;    // Delay to apply so all devices synchronize to slowest
    float measuredLevelDb;       // Acoustic RMS level captured by mic (dBFS)
    float recommendedVolume;     // Volume trim (0.35 .. 1.35) to match perceived loudness
    float confidence;            // 0.0 .. 1.0 matched-filter SNR confidence
    int32_t succeeded;           // 1 if clearly heard by mic, 0 if below noise floor
} WaCalibrationResult;

typedef struct WaAbiSizes {
    int32_t abiVersion;
    int32_t sizeParamBand;
    int32_t sizeEqParams;
    int32_t sizeDspParams;
    int32_t sizeDeviceInfo;
    int32_t sizeOutputSlotConfig;
    int32_t sizeOutputTelemetry;
    int32_t sizeTelemetry;
    int32_t sizeCalibrationResult;
} WaAbiSizes;

#pragma pack(pop)

typedef void* WaEngineHandle;
typedef void (__stdcall *WaDeviceChangeCallback)(void* userContext);

// ABI introspection (for automated C#/C++ layout verification)
WA_API void wa_get_abi_sizes(WaAbiSizes* outSizes);
WA_API void wa_init_default_dsp_params(WaDspParams* outParams);

// Engine lifecycle
WA_API WaEngineHandle wa_create(void);
WA_API void wa_destroy(WaEngineHandle handle);

// preferredVirtualSinkId: NULL/empty = auto-detect VB-CABLE; bufferMs = 10..40 (default 15)
// preferVirtualMode: 1 = use virtual sink if found, 0 = force Direct Loopback mode
WA_API int32_t wa_start(WaEngineHandle handle, const wchar_t* preferredVirtualSinkId, int32_t preferVirtualMode, int32_t bufferMs);
WA_API void wa_stop(WaEngineHandle handle);
WA_API int32_t wa_get_routing_mode(WaEngineHandle handle);

// DSP controls (thread-safe, lock-free atomic swap)
WA_API void wa_set_master_dsp(WaEngineHandle handle, const WaDspParams* params);
WA_API void wa_set_bypass(WaEngineHandle handle, int32_t bypass);

// Analytical EQ + Tone response curve evaluation for UI display
WA_API void wa_compute_eq_curve(const WaDspParams* params, float sampleRate, const float* freqsHz, float* outDb, int32_t count);

// Device enumeration, Windows default switching & multi-device configuration
WA_API int32_t wa_enumerate_devices(WaEngineHandle handle, WaDeviceInfo* outDevices, int32_t maxCount);
WA_API int32_t wa_set_default_device(WaEngineHandle handle, const wchar_t* deviceId, int32_t includeCommunications);
WA_API int32_t wa_get_endpoint_volume(WaEngineHandle handle, const wchar_t* deviceId, float* outVolume01, int32_t* outMuted, float* outBalance);
WA_API int32_t wa_set_endpoint_volume(WaEngineHandle handle, const wchar_t* deviceId, float volume01, int32_t muted, float balance);
WA_API int32_t wa_configure_outputs(WaEngineHandle handle, const WaOutputSlotConfig* slots, int32_t count);

// Sync pulse, acoustic mic calibration & test tone helpers
WA_API void wa_trigger_sync_pulse(WaEngineHandle handle);
WA_API void wa_set_test_tone(WaEngineHandle handle, int32_t enabled, float freqHz, float gainDb);
WA_API int32_t wa_get_microphone_info(WaEngineHandle handle, wchar_t* outMicName, int32_t maxNameLen);
WA_API int32_t wa_run_mic_calibration(WaEngineHandle handle, const wchar_t* const* deviceIds, int32_t deviceCount, WaCalibrationResult* outResults);

// Telemetry (lazy FFT runs only when includeSpectrum == 1) & device hot-plug notification
WA_API void wa_get_telemetry(WaEngineHandle handle, WaTelemetry* outTelemetry, int32_t includeSpectrum);
WA_API void wa_register_device_change_callback(WaEngineHandle handle, WaDeviceChangeCallback callback, void* userContext);

#ifdef __cplusplus
}
#endif
