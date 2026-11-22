#include "DriftResampler.h"
#include <algorithm>
#include <cmath>

namespace winaudio::audio {

void DriftResampler::prepare(double inputSampleRate, double outputSampleRate, double targetBufferMs) noexcept {
    inRate_ = std::max(8000.0, inputSampleRate);
    outRate_ = std::max(8000.0, outputSampleRate);
    nominalRatio_ = inRate_ / outRate_;
    targetBufferFrames_ = std::max(128.0, (targetBufferMs * 0.001) * inRate_);

    // Allocate up to 550 ms delay buffer at output sample rate
    const int maxDelay = static_cast<int>(std::ceil(outRate_ * 0.55)) + 16;
    delayBufSize_ = std::max(1024, maxDelay);
    delayBufL_.assign(delayBufSize_, 0.0f);
    delayBufR_.assign(delayBufSize_, 0.0f);
    delayWriteIdx_ = 0;

    reset();
}

void DriftResampler::reset() noexcept {
    phase_ = 0.0;
    histL_.fill(0.0f);
    histR_.fill(0.0f);
    smoothedFillFrames_ = targetBufferFrames_;
    smoothedFillMs_ = (targetBufferFrames_ / inRate_) * 1000.0;
    piIntegral_ = 0.0;
    driftPpm_ = 0.0;
    primed_ = false;
    underruns_ = 0;
    overruns_ = 0;

    std::fill(delayBufL_.begin(), delayBufL_.end(), 0.0f);
    std::fill(delayBufR_.begin(), delayBufR_.end(), 0.0f);
    delayWriteIdx_ = 0;
    currentDelaySamples_ = targetDelaySamples_;
    currentDelayMs_ = (currentDelaySamples_ / outRate_) * 1000.0;
}

void DriftResampler::setTargetBufferMs(double targetBufferMs) noexcept {
    targetBufferFrames_ = std::max(128.0, (std::clamp(targetBufferMs, 20.0, 250.0) * 0.001) * inRate_);
}

void DriftResampler::setDelayMs(double delayMs) noexcept {
    const double clampedMs = std::clamp(delayMs, 0.0, 500.0);
    targetDelaySamples_ = (clampedMs * 0.001) * outRate_;
}

void DriftResampler::pull(StereoRingBuffer& ring, float* interleavedStereoOut, int outputFrames) noexcept {
    if (!interleavedStereoOut || outputFrames <= 0) return;

    uint32_t avail = ring.availableRead();

    // Wait until buffer reaches target fill on startup or after an underrun so PI starts at 0 ppm drift
    if (!primed_) {
        const uint32_t primeThreshold = static_cast<uint32_t>(targetBufferFrames_);
        if (avail < primeThreshold) {
            std::fill(interleavedStereoOut, interleavedStereoOut + outputFrames * 2, 0.0f);
            smoothedFillFrames_ = static_cast<double>(avail);
            smoothedFillMs_ = (static_cast<double>(avail) / inRate_) * 1000.0;
            return;
        }
        // Pre-seed 4-point Hermite history
        for (int i = 0; i < 4; ++i) {
            float l = 0.0f, r = 0.0f;
            ring.popFrame(l, r);
            histL_[i] = l;
            histR_[i] = r;
        }
        phase_ = 0.0;
        piIntegral_ = 0.0;
        smoothedFillFrames_ = targetBufferFrames_;
        primed_ = true;
        avail = ring.availableRead();
    }

    // Extreme overrun protection (e.g. thread pause/sleep wake) -> trim cleanly back to target
    const uint32_t maxHealthyFrames = static_cast<uint32_t>(targetBufferFrames_ * 4.0) + 2048;
    if (avail > maxHealthyFrames) {
        const uint32_t drop = avail - static_cast<uint32_t>(targetBufferFrames_);
        ring.skip(drop);
        ++overruns_;
        avail = ring.availableRead();
    }

    // Smooth fill level measurement (1.5s smoothing completely ignores 10-20ms WASAPI/BT packet jitter)
    const double dtSec = static_cast<double>(outputFrames) / outRate_;
    const double fillAlpha = 1.0 - std::exp(-dtSec / 1.5);
    smoothedFillFrames_ += fillAlpha * (static_cast<double>(avail) - smoothedFillFrames_);
    smoothedFillMs_ = (smoothedFillFrames_ / inRate_) * 1000.0;

    // Heavily damped broadcast-grade PI Clock-Drift Controller:
    // Smoothly tracks real hardware crystal clock drift (+/- 150 ppm) with zero audible pitch wobble.
    const double errorNorm = (smoothedFillFrames_ - targetBufferFrames_) / std::max(512.0, targetBufferFrames_);
    constexpr double kP = 0.00030;  // Gentle proportional response (zero jitter-induced FM modulation)
    constexpr double kI = 0.00012;  // Integral term locks onto steady-state crystal clock offset

    piIntegral_ = std::clamp(piIntegral_ + errorNorm * dtSec, -1.5, 1.5);
    double ratioAdjust = std::clamp(kP * errorNorm + kI * piIntegral_, -0.0020, 0.0020); // max +/-0.2%
    driftPpm_ = ratioAdjust * 1e6;

    const double step = nominalRatio_ * (1.0 + ratioAdjust);

    // Smoothly slew delay length (max 15 samples per block -> subtle pitch-free adjustment)
    const double delaySlewPerSample = 0.005; // smooth fractional delay slew

    bool hitUnderrun = false;
    int underrunSamples = 0;
    for (int i = 0; i < outputFrames; ++i) {
        // Advance input stream as needed for Hermite 4-point window [hist[0], hist[1], hist[2], hist[3]]
        while (phase_ >= 1.0) {
            float nextL = 0.0f, nextR = 0.0f;
            if (ring.popFrame(nextL, nextR)) {
                histL_[0] = histL_[1];
                histL_[1] = histL_[2];
                histL_[2] = histL_[3];
                histL_[3] = nextL;

                histR_[0] = histR_[1];
                histR_[1] = histR_[2];
                histR_[2] = histR_[3];
                histR_[3] = nextR;
            } else {
                // Graceful underrun fade toward zero (no click or buzz)
                hitUnderrun = true;
                ++underrunSamples;
                histL_[0] = histL_[1];
                histL_[1] = histL_[2];
                histL_[2] = histL_[3];
                histL_[3] *= 0.85f;

                histR_[0] = histR_[1];
                histR_[1] = histR_[2];
                histR_[2] = histR_[3];
                histR_[3] *= 0.85f;
            }
            phase_ -= 1.0;
        }

        float sampleL = hermite4(histL_[0], histL_[1], histL_[2], histL_[3], phase_);
        float sampleR = hermite4(histR_[0], histR_[1], histR_[2], histR_[3], phase_);
        phase_ += step;

        // Per-device fractional sync delay line (0 .. 500 ms)
        delayBufL_[delayWriteIdx_] = sampleL;
        delayBufR_[delayWriteIdx_] = sampleR;

        const double diff = targetDelaySamples_ - currentDelaySamples_;
        if (std::abs(diff) > 1e-4) {
            currentDelaySamples_ += std::clamp(diff, -delaySlewPerSample, delaySlewPerSample);
        }

        if (currentDelaySamples_ > 0.5) {
            double readPos = static_cast<double>(delayWriteIdx_) - currentDelaySamples_;
            while (readPos < 0.0) readPos += delayBufSize_;
            int idx0 = static_cast<int>(readPos) % delayBufSize_;
            int idx1 = (idx0 + 1) % delayBufSize_;
            float frac = static_cast<float>(readPos - std::floor(readPos));

            sampleL = delayBufL_[idx0] + frac * (delayBufL_[idx1] - delayBufL_[idx0]);
            sampleR = delayBufR_[idx0] + frac * (delayBufR_[idx1] - delayBufR_[idx0]);
        }

        delayWriteIdx_++;
        if (delayWriteIdx_ >= delayBufSize_) delayWriteIdx_ = 0;

        interleavedStereoOut[i * 2] = sampleL;
        interleavedStereoOut[i * 2 + 1] = sampleR;
    }

    currentDelayMs_ = (currentDelaySamples_ / outRate_) * 1000.0;
    if (hitUnderrun) {
        ++underruns_;
        // Re-prime cleanly back to target fill so the buffer never stays pinned at 0 frames buzzing!
        if (underrunSamples >= 4) {
            primed_ = false;
            piIntegral_ = 0.0;
        }
    }
}

} // namespace winaudio::audio
