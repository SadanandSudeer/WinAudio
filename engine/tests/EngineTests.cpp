#include "WinAudioABI.h"
#include "dsp/MasterDspChain.h"
#include "dsp/DspMath.h"
#include "audio/RingBuffer.h"
#include "audio/DriftResampler.h"
#include "audio/AcousticCalibrator.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace winaudio::dsp;
using namespace winaudio::audio;

namespace {

int gPassed = 0;
int gFailed = 0;

void check(bool cond, const char* name, const std::string& detail = "") {
    if (cond) {
        ++gPassed;
        std::printf("[PASS] %s\n", name);
    } else {
        ++gFailed;
        std::printf("[FAIL] %s %s\n", name, detail.c_str());
    }
}

// Measure steady-state gain in dB at freqHz through MasterDspChain
double measureSineGainDb(MasterDspChain& chain, double sr, double freqHz, double amp = 0.1) {
    const int settleFrames = static_cast<int>(sr * 0.25);
    const int measFrames = static_cast<int>(sr * 0.20);
    std::vector<float> buf(static_cast<size_t>(settleFrames + measFrames) * 2);

    for (int i = 0; i < settleFrames + measFrames; ++i) {
        float s = static_cast<float>(amp * std::sin(kTwoPi * freqHz * i / sr));
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    // Process in realistic 480-frame chunks (10ms)
    for (int off = 0; off < settleFrames + measFrames; off += 480) {
        int n = std::min(480, (settleFrames + measFrames) - off);
        chain.processBlock(buf.data() + off * 2, n);
    }

    double sumSq = 0.0;
    for (int i = settleFrames; i < settleFrames + measFrames; ++i) {
        double v = buf[i * 2];
        sumSq += v * v;
    }
    const double outRms = std::sqrt(sumSq / measFrames);
    const double inRms = amp / std::sqrt(2.0);
    return 20.0 * std::log10(std::max(1e-12, outRms / inRms));
}

void testEqualizerAccuracy() {
    constexpr double sr = 48000.0;

    // 10-band peaking EQ at 1000 Hz (+6.0 dB)
    {
        MasterDspChain chain;
        chain.prepare(sr);
        auto p = MasterDspChain::defaultParams();
        p.maximizerEnabled = 0;
        p.eq.enabled = 1;
        p.eq.mode = WA_EQ_10BAND;
        p.eq.graphicGainsDb[5] = 6.0f; // 1000 Hz
        chain.setParams(p);

        double g1k = measureSineGainDb(chain, sr, 1000.0);
        double g100 = measureSineGainDb(chain, sr, 100.0);
        check(std::abs(g1k - 6.0) < 0.25, "EQ 10-band +6dB @ 1kHz accuracy", "got=" + std::to_string(g1k));
        check(std::abs(g100) < 0.35, "EQ 10-band off-band @ 100Hz near 0dB", "got=" + std::to_string(g100));

        // Analytical curve matches measured response
        float f = 1000.0f, outDb = 0.0f;
        MasterDspChain::computeAnalyticalCurve(p, static_cast<float>(sr), &f, &outDb, 1);
        check(std::abs(outDb - 6.0f) < 0.1f, "EQ analytical curve matches +6dB @ 1kHz", "got=" + std::to_string(outDb));
    }

    // 31-band EQ at 250 Hz (-9.0 dB)
    {
        MasterDspChain chain;
        chain.prepare(sr);
        auto p = MasterDspChain::defaultParams();
        p.maximizerEnabled = 0;
        p.eq.enabled = 1;
        p.eq.mode = WA_EQ_31BAND;
        p.eq.graphicGainsDb[11] = -9.0f; // 250 Hz
        chain.setParams(p);

        double g250 = measureSineGainDb(chain, sr, 250.0);
        check(std::abs(g250 - (-9.0)) < 0.25, "EQ 31-band -9dB @ 250Hz accuracy", "got=" + std::to_string(g250));
    }

    // Semi-parametric Notch & LowShelf
    {
        MasterDspChain chain;
        chain.prepare(sr);
        auto p = MasterDspChain::defaultParams();
        p.maximizerEnabled = 0;
        p.eq.enabled = 1;
        p.eq.mode = WA_EQ_PARAMETRIC;
        for (int i = 0; i < WA_MAX_PARAM_BANDS; ++i) p.eq.paramBands[i].enabled = 0;
        p.eq.paramBands[0].enabled = 1;
        p.eq.paramBands[0].filterType = WA_FILTER_NOTCH;
        p.eq.paramBands[0].frequencyHz = 2000.0f;
        p.eq.paramBands[0].q = 4.0f;
        chain.setParams(p);

        double gNotch = measureSineGainDb(chain, sr, 2000.0);
        check(gNotch < -35.0, "Semi-parametric Notch @ 2kHz deep attenuation", "got=" + std::to_string(gNotch));
    }
}

void testToneBoostersAndExciter() {
    constexpr double sr = 48000.0;
    MasterDspChain chain;
    chain.prepare(sr);
    auto p = MasterDspChain::defaultParams();
    p.maximizerEnabled = 0;
    p.eq.enabled = 0;
    p.bassEnabled = 1;
    p.bassBoostDb = 8.0f;
    p.bassFreqHz = 100.0f;
    p.bassExciterMix = 0.0f;
    p.trebleEnabled = 1;
    p.trebleBoostDb = 6.0f;
    p.trebleFreqHz = 6000.0f;
    p.clarityBoostDb = 3.0f;
    chain.setParams(p);

    double g50 = measureSineGainDb(chain, sr, 50.0, 0.05);
    double g12k = measureSineGainDb(chain, sr, 12000.0, 0.05);
    check(g50 > 5.5 && g50 < 9.0, "Bass booster low-shelf boosts 50Hz", "got=" + std::to_string(g50));
    check(g12k > 4.5 && g12k < 8.0, "Treble booster high-shelf boosts 12kHz", "got=" + std::to_string(g12k));

    // Psychoacoustic bass exciter: feed pure 75 Hz sine, measure generated 2nd/3rd harmonic energy at 150 Hz / 225 Hz
    p.bassBoostDb = 0.0f;
    p.bassExciterMix = 1.0f;
    p.trebleEnabled = 0;
    chain.reset();
    chain.setParams(p);

    const int N = 24000;
    std::vector<float> buf(N * 2);
    for (int i = 0; i < N; ++i) {
        float s = static_cast<float>(0.35 * std::sin(kTwoPi * 75.0 * i / sr));
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }
    chain.processBlock(buf.data(), N);

    // Goertzel / DFT magnitude at 2nd harmonic (150 Hz) and 3rd harmonic (225 Hz)
    auto dftMag = [&](double freq) {
        double re = 0.0, im = 0.0;
        int start = N / 2;
        int len = N - start;
        for (int i = start; i < N; ++i) {
            double w = kTwoPi * freq * i / sr;
            re += buf[i * 2] * std::cos(w);
            im -= buf[i * 2] * std::sin(w);
        }
        return 2.0 * std::sqrt(re * re + im * im) / len;
    };
    double h2 = dftMag(150.0);
    double h3 = dftMag(225.0);
    check((h2 + h3) > 0.015, "Psychoacoustic bass exciter generates upper harmonics (150/225Hz)", "h2+h3=" + std::to_string(h2 + h3));
}

void testMaximizerBrickwall() {
    constexpr double sr = 48000.0;
    MasterDspChain chain;
    chain.prepare(sr);

    auto p = MasterDspChain::defaultParams();
    p.eq.enabled = 0;
    p.maximizerEnabled = 1;
    p.maximizerBoostDb = 18.0f; // +18 dB (~8x gain!)
    p.maximizerCeilingDb = -0.3f;
    p.maximizerSoftClip = 0.25f;
    chain.setParams(p);

    const float ceilingLin = dbToLinear(-0.3f) + 1e-4f;
    const int N = 48000;
    std::vector<float> buf(N * 2);
    for (int i = 0; i < N; ++i) {
        // Mix of hot sine wave + sudden impulse spike at sample 24000
        float s = static_cast<float>(0.65 * std::sin(kTwoPi * 440.0 * i / sr));
        if (i == 24000) s = 1.0f;
        buf[i * 2] = s;
        buf[i * 2 + 1] = -s;
    }

    for (int off = 0; off < N; off += 480) {
        chain.processBlock(buf.data() + off * 2, 480);
    }

    float maxPeak = 0.0f;
    bool allFinite = true;
    for (float v : buf) {
        if (!std::isfinite(v)) allFinite = false;
        maxPeak = std::max(maxPeak, std::abs(v));
    }

    auto meters = chain.getMeters();
    check(allFinite, "Maximizer produces 100% finite samples");
    check(maxPeak <= ceilingLin, "Maximizer enforces -0.3 dBFS brickwall ceiling on +18dB hot signal + impulse",
          "maxPeak=" + std::to_string(maxPeak) + " ceil=" + std::to_string(ceilingLin));
    check(meters.limiterGrDb < -6.0f, "Maximizer reports active gain reduction telemetry", "gr=" + std::to_string(meters.limiterGrDb));
}

void testLoudnessNormalizationAndGate() {
    constexpr double sr = 48000.0;
    MasterDspChain chain;
    chain.prepare(sr);

    auto p = MasterDspChain::defaultParams();
    p.eq.enabled = 0;
    p.maximizerEnabled = 0;
    p.loudnessEnabled = 1;
    p.loudnessTargetLufs = -14.0f;
    p.loudnessStrength = 0.9f;
    p.loudnessMaxGainDb = 12.0f;
    chain.setParams(p);

    // 1. Quiet speech-like tone at -30 dBFS should be boosted significantly
    const int N = static_cast<int>(sr * 4.0);
    std::vector<float> quiet(N * 2);
    const float ampQuiet = dbToLinear(-30.0f);
    for (int i = 0; i < N; ++i) {
        float s = static_cast<float>(ampQuiet * std::sin(kTwoPi * 1000.0 * i / sr));
        quiet[i * 2] = quiet[i * 2 + 1] = s;
    }
    for (int off = 0; off < N; off += 480) chain.processBlock(quiet.data() + off * 2, 480);
    float gainOnQuiet = chain.getMeters().loudnessGainDb;
    check(gainOnQuiet > 6.0f, "Loudness normalizer boosts quiet -30dBFS audio", "gainDb=" + std::to_string(gainOnQuiet));

    // 2. Near-silence (-70 dBFS noise floor) must be gated and return toward 0 dB (NOT boosted!)
    chain.reset();
    const float ampNoise = dbToLinear(-70.0f);
    for (int i = 0; i < N; ++i) {
        float s = static_cast<float>(ampNoise * std::sin(kTwoPi * 1000.0 * i / sr));
        quiet[i * 2] = quiet[i * 2 + 1] = s;
    }
    for (int off = 0; off < N; off += 480) chain.processBlock(quiet.data() + off * 2, 480);
    float gainOnSilence = chain.getMeters().loudnessGainDb;
    check(std::abs(gainOnSilence) < 1.0f, "Loudness normalizer noise-gates -70dBFS silence", "gainDb=" + std::to_string(gainOnSilence));
}

void testSpatialSuiteAndStability() {
    constexpr double sr = 48000.0;

    // 1. Stereo Widener preserves bass mono at 80 Hz while widening 2 kHz
    {
        MasterDspChain chain;
        chain.prepare(sr);
        auto p = MasterDspChain::defaultParams();
        p.eq.enabled = 0;
        p.maximizerEnabled = 0;
        p.widenerEnabled = 1;
        p.stereoWidth = 1.8f;
        p.bassMonoFreqHz = 180.0f;
        chain.setParams(p);

        // Hard-panned left 80 Hz bass tone -> should collapse near mono (L ~= R)
        const int N = 24000;
        std::vector<float> buf(N * 2);
        for (int i = 0; i < N; ++i) {
            buf[i * 2] = static_cast<float>(0.3 * std::sin(kTwoPi * 80.0 * i / sr));
            buf[i * 2 + 1] = 0.0f;
        }
        chain.processBlock(buf.data(), N);
        double sideSq = 0.0, midSq = 0.0;
        for (int i = N / 2; i < N; ++i) {
            double l = buf[i * 2], r = buf[i * 2 + 1];
            double m = 0.5 * (l + r), s = 0.5 * (l - r);
            midSq += m * m;
            sideSq += s * s;
        }
        check(sideSq < midSq * 0.15, "Stereo widener keeps 80Hz sub-bass centered/mono",
              "side/mid=" + std::to_string(sideSq / std::max(1e-12, midSq)));
    }

    // 2. Virtual Surround + Max FDN Ambience stability over 10 seconds (zero runaway / NaN)
    {
        MasterDspChain chain;
        chain.prepare(sr);
        auto p = MasterDspChain::defaultParams();
        p.surroundMode = WA_SURROUND_HEADPHONES;
        p.surroundDepth = 1.0f;
        p.ambienceEnabled = 1;
        p.ambienceRoomSize = 1.0f;
        p.ambienceDamping = 0.2f;
        p.ambienceMix = 0.65f;
        chain.setParams(p);

        const int totalFrames = static_cast<int>(sr * 10.0);
        std::vector<float> chunk(480 * 2);
        float maxVal = 0.0f;
        bool finite = true;
        for (int off = 0; off < totalFrames; off += 480) {
            for (int i = 0; i < 480; ++i) {
                int idx = off + i;
                float s = (idx < 4800) ? static_cast<float>(0.4 * std::sin(kTwoPi * 523.25 * idx / sr)) : 0.0f;
                chunk[i * 2] = s;
                chunk[i * 2 + 1] = s * 0.7f;
            }
            chain.processBlock(chunk.data(), 480);
            for (float v : chunk) {
                if (!std::isfinite(v)) finite = false;
                maxVal = std::max(maxVal, std::abs(v));
            }
        }
        // Last chunk (after 9.9 seconds of silence input) must have decayed cleanly
        float tailPeak = 0.0f;
        for (float v : chunk) tailPeak = std::max(tailPeak, std::abs(v));
        check(finite && maxVal <= 1.0f, "Virtual Surround + FDN Ambience remains bounded over 10s", "max=" + std::to_string(maxVal));
        check(tailPeak < 0.01f, "FDN Ambience tail decays cleanly without self-oscillation", "tail=" + std::to_string(tailPeak));
    }
}

void testClickFreeBypass() {
    constexpr double sr = 48000.0;
    MasterDspChain chain;
    chain.prepare(sr);
    auto p = MasterDspChain::defaultParams();
    p.preAmpDb = 6.0f;
    chain.setParams(p);

    std::vector<float> block(480 * 2);
    float prevL = 0.0f;
    float maxDelta = 0.0f;

    for (int blk = 0; blk < 20; ++blk) {
        if (blk == 10) chain.setBypass(true); // Toggle bypass mid-sine-wave!
        for (int i = 0; i < 480; ++i) {
            int idx = blk * 480 + i;
            float s = static_cast<float>(0.3 * std::sin(kTwoPi * 220.0 * idx / sr));
            block[i * 2] = block[i * 2 + 1] = s;
        }
        chain.processBlock(block.data(), 480);
        if (blk >= 9) {
            for (int i = 0; i < 480; ++i) {
                float cur = block[i * 2];
                if (blk == 10 || (blk == 9 && i > 0)) {
                    maxDelta = std::max(maxDelta, std::abs(cur - prevL));
                }
                prevL = cur;
            }
        }
    }
    check(maxDelta < 0.05f, "Click-free 5ms bypass toggle has no abrupt discontinuity", "maxDelta=" + std::to_string(maxDelta));
}

void testRingBufferConcurrency() {
    StereoRingBuffer ring(8192);
    constexpr uint32_t totalFrames = 2000000;
    std::atomic<bool> integrityOk{ true };

    std::thread producer([&]() {
        std::vector<float> chunk(256 * 2);
        uint32_t produced = 0;
        while (produced < totalFrames) {
            uint32_t n = std::min<uint32_t>(256, totalFrames - produced);
            for (uint32_t i = 0; i < n; ++i) {
                float val = static_cast<float>((produced + i) % 65536);
                chunk[i * 2] = val;
                chunk[i * 2 + 1] = -val;
            }
            uint32_t written = 0;
            while (written < n) {
                written += ring.write(chunk.data() + written * 2, n - written);
            }
            produced += n;
        }
    });

    std::thread consumer([&]() {
        std::vector<float> chunk(192 * 2);
        uint32_t consumed = 0;
        while (consumed < totalFrames) {
            uint32_t got = ring.read(chunk.data(), 192);
            for (uint32_t i = 0; i < got; ++i) {
                float expected = static_cast<float>((consumed + i) % 65536);
                if (chunk[i * 2] != expected || chunk[i * 2 + 1] != -expected) {
                    integrityOk.store(false, std::memory_order_relaxed);
                }
            }
            consumed += got;
        }
    });

    producer.join();
    consumer.join();
    check(integrityOk.load(), "Lock-free SPSC RingBuffer 2M-frame concurrent stress test");
}

void testAsrcDriftAndSyncDelay() {
    // Simulate 48 kHz engine feeding a 44.1 kHz Bluetooth sink with +150 ppm clock drift over 60 seconds!
    StereoRingBuffer ring(32768);
    DriftResampler resampler;
    resampler.prepare(48000.0, 44100.0, 30.0);
    resampler.setDelayMs(150.0); // Wired+BT sync delay test

    std::vector<float> inChunk(480 * 2);   // 10 ms @ 48 kHz
    std::vector<float> outChunk(512 * 2);
    double outAccum = 0.0;
    const double outFramesPer10Ms = 441.0 * (1.0 - 0.000150); // Output clock running 150 ppm slower!

    for (int step = 0; step < 6000; ++step) { // 60 seconds simulated
        for (int i = 0; i < 480; ++i) {
            double t = (step * 480 + i) / 48000.0;
            float s = static_cast<float>(0.4 * std::sin(kTwoPi * 440.0 * t));
            inChunk[i * 2] = inChunk[i * 2 + 1] = s;
        }
        ring.write(inChunk.data(), 480);

        outAccum += outFramesPer10Ms;
        int toPull = static_cast<int>(outAccum);
        outAccum -= toPull;
        resampler.pull(ring, outChunk.data(), toPull);
    }

    float fillMs = resampler.smoothedFillMs();
    check(resampler.underrunCount() == 0 && resampler.overrunCount() == 0,
          "ASRC + PI drift controller zero underruns/overruns over 60s @ 48k->44.1k +150ppm",
          "underruns=" + std::to_string(resampler.underrunCount()) + " overruns=" + std::to_string(resampler.overrunCount()));
    check(std::abs(fillMs - 30.0f) < 3.0f,
          "ASRC PI controller converges buffer fill tightly around 30ms target",
          "fillMs=" + std::to_string(fillMs) + " driftPpm=" + std::to_string(resampler.driftPpm()));
    check(std::abs(resampler.appliedDelayMs() - 150.0) < 2.0,
          "Per-device Sync Delay line converges cleanly to 150ms",
          "appliedMs=" + std::to_string(resampler.appliedDelayMs()));

    // Spectral purity (SINAD) test for exact JBL Bluetooth (44.1 kHz) -> Wired/HDMI (48 kHz) sync path!
    {
        StereoRingBuffer ringBt(32768);
        DriftResampler rsBt;
        rsBt.prepare(44100.0, 48000.0, 45.0);

        std::vector<float> inPkt(441 * 2);   // 10 ms @ 44.1 kHz
        std::vector<float> outPkt(480 * 2);  // 10 ms @ 48.0 kHz
        std::vector<float> captured;
        captured.reserve(48000 * 2);

        for (int step = 0; step < 200; ++step) { // 2.0 seconds
            for (int i = 0; i < 441; ++i) {
                double t = (step * 441 + i) / 44100.0;
                float s = static_cast<float>(0.5 * std::sin(kTwoPi * 1000.0 * t));
                inPkt[i * 2] = inPkt[i * 2 + 1] = s;
            }
            ringBt.write(inPkt.data(), 441);
            rsBt.pull(ringBt, outPkt.data(), 480);
            if (step >= 100) { // Measure steady-state second [1.0s .. 2.0s]
                for (int i = 0; i < 480; ++i) captured.push_back(outPkt[i * 2]);
            }
        }

        // Fit 1000 Hz fundamental (accounting for tiny driftPpm) over a 100ms window and verify SINAD > 60 dB
        const int M = 4800;
        const int startOff = static_cast<int>(captured.size()) - M;
        const double actualFreq = 1000.0 * (1.0 + rsBt.driftPpm() * 1e-6);
        double cosSum = 0.0, sinSum = 0.0;
        for (int i = 0; i < M; ++i) {
            double w = kTwoPi * actualFreq * i / 48000.0;
            cosSum += captured[startOff + i] * std::cos(w);
            sinSum += captured[startOff + i] * std::sin(w);
        }
        double a = 2.0 * cosSum / M;
        double b = 2.0 * sinSum / M;
        double sigPow = 0.5 * (a * a + b * b);
        double noisePow = 0.0;
        for (int i = 0; i < M; ++i) {
            double w = kTwoPi * actualFreq * i / 48000.0;
            double fit = a * std::cos(w) + b * std::sin(w);
            double err = captured[startOff + i] - fit;
            noisePow += err * err;
        }
        noisePow /= M;
        double sinadDb = 10.0 * std::log10(std::max(1e-12, sigPow) / std::max(1e-12, noisePow));
        check(rsBt.underrunCount() == 0 && sinadDb > 60.0 && std::abs(rsBt.driftPpm()) < 80.0f,
              "44.1kHz BT -> 48kHz Wired ASRC spectral purity (zero tone/buzz, SINAD > 60 dB, <80ppm)",
              "sinadDb=" + std::to_string(sinadDb) + " driftPpm=" + std::to_string(rsBt.driftPpm()));
    }

    // 4-Device Simultaneous Multi-Output Stress Test (2 jittery BT A2DP sinks + 1 Wired 48k + 1 Digital 192k)
    {
        StereoRingBuffer rBt1(65536), rBt2(65536), rWired(65536), rDig(65536);
        DriftResampler dBt1, dBt2, dWired, dDig;
        dBt1.prepare(48000.0, 44100.0, 100.0);
        dBt2.prepare(48000.0, 48000.0, 100.0);
        dWired.prepare(48000.0, 48000.0, 60.0);
        dDig.prepare(48000.0, 192000.0, 60.0);

        std::vector<float> inPkt(480 * 2, 0.25f);
        std::vector<float> outScratch(4096 * 2);

        // Simulate 30 seconds where BT sinks pull in erratic 10ms..40ms TDM bursts!
        int bt1Accum = 0, bt2Accum = 0;
        for (int step = 0; step < 3000; ++step) {
            rBt1.write(inPkt.data(), 480);
            rBt2.write(inPkt.data(), 480);
            rWired.write(inPkt.data(), 480);
            rDig.write(inPkt.data(), 480);

            dWired.pull(rWired, outScratch.data(), 480);
            dDig.pull(rDig, outScratch.data(), 1920);

            bt1Accum += 441;
            if ((step % 4) == 3) { // 40ms Bluetooth TDM burst!
                dBt1.pull(rBt1, outScratch.data(), bt1Accum);
                bt1Accum = 0;
            }
            bt2Accum += 480;
            if ((step % 3) == 2) { // 30ms Bluetooth TDM burst!
                dBt2.pull(rBt2, outScratch.data(), bt2Accum);
                bt2Accum = 0;
            }
        }

        const uint32_t totalUnderruns = dBt1.underrunCount() + dBt2.underrunCount() + dWired.underrunCount() + dDig.underrunCount();
        const uint32_t totalOverruns = dBt1.overrunCount() + dBt2.overrunCount() + dWired.overrunCount() + dDig.overrunCount();
        check(totalUnderruns == 0 && totalOverruns == 0,
              "4-Device simultaneous Multi-Out (2 bursty BT + Wired + 192k Digital) zero glitches over 30s",
              "underruns=" + std::to_string(totalUnderruns) + " overruns=" + std::to_string(totalOverruns));
    }
}

void testAcousticMicCalibrationDsp() {
    constexpr double sr = 48000.0;
    auto chirp = AcousticCalibrator::generateLogChirp(sr, 0.4f);

    // Synthesize a 1.0s mic recording with chirp triggered at 100ms + true acoustic delay of 142.5ms + room noise!
    auto makeNoisyMicRecording = [&](double trueDelayMs, float gain) {
        std::vector<float> rec(static_cast<size_t>(sr * 1.0), 0.0f);
        uint32_t rng = 0x12345678u;
        for (float& s : rec) {
            rng = rng * 1664525u + 1013904223u;
            float noise = ((static_cast<float>(rng >> 8) / 16777216.0f) * 2.0f - 1.0f) * 0.03f; // realistic room noise
            s = noise;
        }
        const size_t insertAt = static_cast<size_t>(std::round(((100.0 + trueDelayMs) * 0.001) * sr));
        for (size_t i = 0; i < chirp.size() && insertAt + i < rec.size(); ++i) {
            rec[insertAt + i] += chirp[i] * gain;
        }
        return rec;
    };

    auto recFast = makeNoisyMicRecording(35.0, 0.8f);   // Wired speaker (35ms, louder)
    auto recSlow = makeNoisyMicRecording(187.5, 0.45f); // Bluetooth speaker (187.5ms, quieter)

    auto aFast = AcousticCalibrator::analyzeRecording(recFast, sr, 0.100, 500.0);
    auto aSlow = AcousticCalibrator::analyzeRecording(recSlow, sr, 0.100, 500.0);

    check(aFast.detected && std::abs(aFast.latencyMs - 35.0f) < 1.0f,
          "Acoustic chirp calibrator measures 35.0ms delay under room noise (<1ms error)",
          "got=" + std::to_string(aFast.latencyMs) + " conf=" + std::to_string(aFast.confidence));
    check(aSlow.detected && std::abs(aSlow.latencyMs - 187.5f) < 1.0f,
          "Acoustic chirp calibrator measures 187.5ms BT delay under room noise (<1ms error)",
          "got=" + std::to_string(aSlow.latencyMs) + " conf=" + std::to_string(aSlow.confidence));

    // Verify relative delay alignment & loudness balancing
    std::vector<WaCalibrationResult> results(2);
    results[0].succeeded = 1; results[0].measuredLatencyMs = aFast.latencyMs; results[0].measuredLevelDb = aFast.rmsDb;
    results[1].succeeded = 1; results[1].measuredLatencyMs = aSlow.latencyMs; results[1].measuredLevelDb = aSlow.rmsDb;
    AcousticCalibrator::computeRelativeAdjustments(results);

    check(std::abs(results[0].recommendedDelayMs - 152.5f) < 1.5f && results[1].recommendedDelayMs == 0.0f,
          "Relative delay alignment delays faster speaker by ~153ms to sync with BT speaker",
          "fastDelay=" + std::to_string(results[0].recommendedDelayMs));
    check(results[0].recommendedVolume < results[1].recommendedVolume,
          "Relative loudness calibration trims louder speaker and boosts quieter speaker");

    // Live hardware microphone detection check
    wchar_t micName[WA_DEVICE_NAME_LEN]{};
    int32_t hasMic = wa_get_microphone_info(nullptr, micName, WA_DEVICE_NAME_LEN);
    std::wprintf(L"  [INFO] Live active microphone detected: %s (%s)\n",
                 hasMic ? L"YES" : L"NO", hasMic ? micName : L"none");
    check(hasMic == 0 || hasMic == 1, "wa_get_microphone_info executes cleanly against host Windows WASAPI");
}

void testLiveWasapiAndAbiExports() {
    WaAbiSizes sizes{};
    wa_get_abi_sizes(&sizes);
    check(sizes.abiVersion == WA_ABI_VERSION && sizes.sizeDspParams == static_cast<int32_t>(sizeof(WaDspParams)),
          "C-ABI wa_get_abi_sizes matches compiled struct layout");

    WaEngineHandle eng = wa_create();
    check(eng != nullptr, "wa_create returns valid native engine handle");

    std::vector<WaDeviceInfo> devices(WA_MAX_DEVICES);
    int32_t count = wa_enumerate_devices(eng, devices.data(), WA_MAX_DEVICES);
    std::printf("  [INFO] Live Windows WASAPI active render devices detected: %d\n", count);
    for (int32_t i = 0; i < count; ++i) {
        std::wprintf(L"         #%d: %s (transport=%d, %d Hz, %d ch, default=%d, virtual=%d)\n",
                     i + 1, devices[i].name, devices[i].transport,
                     devices[i].sampleRate, devices[i].channels,
                     devices[i].isDefault, devices[i].isVirtualSink);
    }
    check(count >= 0, "wa_enumerate_devices executes cleanly against host Windows WASAPI");

    if (count > 0) {
        const wchar_t* defId = devices[0].id;
        for (int32_t i = 0; i < count; ++i) {
            if (devices[i].isDefault) { defId = devices[i].id; break; }
        }
        float vol = -1.0f, bal = 99.0f;
        int32_t muted = -1;
        int32_t gotVol = wa_get_endpoint_volume(eng, defId, &vol, &muted, &bal);
        check(gotVol == 1 && vol >= 0.0f && vol <= 1.0f && bal >= -1.0f && bal <= 1.0f && (muted == 0 || muted == 1),
              "wa_get_endpoint_volume reads valid Windows Core Audio volume/mute/balance",
              "vol=" + std::to_string(vol) + " muted=" + std::to_string(muted) + " bal=" + std::to_string(bal));
    }

    WaDspParams defParams{};
    wa_init_default_dsp_params(&defParams);
    wa_set_master_dsp(eng, &defParams);

    WaTelemetry tel{};
    wa_get_telemetry(eng, &tel, 1);
    check(tel.routingMode == WA_ROUTING_STOPPED, "Stopped engine reports WA_ROUTING_STOPPED telemetry cleanly");

    wa_destroy(eng);
}

} // namespace

int main() {
    std::printf("=== WinAudio C++20 Engine & DSP Automated Test Suite ===\n");
    testEqualizerAccuracy();
    testToneBoostersAndExciter();
    testMaximizerBrickwall();
    testLoudnessNormalizationAndGate();
    testSpatialSuiteAndStability();
    testClickFreeBypass();
    testRingBufferConcurrency();
    testAsrcDriftAndSyncDelay();
    testAcousticMicCalibrationDsp();
    testLiveWasapiAndAbiExports();

    std::printf("\nSummary: %d passed, %d failed\n", gPassed, gFailed);
    return (gFailed == 0) ? 0 : 1;
}
