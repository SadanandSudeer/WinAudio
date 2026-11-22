#include "SpectrumAnalyzer.h"
#include "DspMath.h"
#include <cmath>
#include <complex>

namespace winaudio::dsp {

void SpectrumAnalyzer::prepare(double sampleRate) noexcept {
    sampleRate_ = std::max(8000.0, sampleRate);
    for (int i = 0; i < kFftSize; ++i) {
        window_[i] = static_cast<float>(0.5 * (1.0 - std::cos(kTwoPi * i / (kFftSize - 1))));
    }
    reset();
}

void SpectrumAnalyzer::reset() noexcept {
    ring_.fill(0.0f);
    writePos_.store(0, std::memory_order_relaxed);
    smoothedDb_.fill(-90.0f);
}

void SpectrumAnalyzer::computeSpectrumDb(float outBinsDb[WA_SPECTRUM_BINS]) noexcept {
    std::array<std::complex<float>, kFftSize> fft{};
    const uint32_t endPos = writePos_.load(std::memory_order_acquire);

    for (int i = 0; i < kFftSize; ++i) {
        uint32_t idx = (endPos - kFftSize + i) & (kFftSize - 1);
        fft[i] = std::complex<float>(ring_[idx] * window_[i], 0.0f);
    }

    // Bit-reversal permutation
    int j = 0;
    for (int i = 1; i < kFftSize; ++i) {
        int bit = kFftSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(fft[i], fft[j]);
    }

    // Iterative Cooley-Tukey Radix-2 FFT
    for (int len = 2; len <= kFftSize; len <<= 1) {
        const float angle = static_cast<float>(-kTwoPi / len);
        const std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (int i = 0; i < kFftSize; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            const int half = len >> 1;
            for (int k = 0; k < half; ++k) {
                auto u = fft[i + k];
                auto v = fft[i + k + half] * w;
                fft[i + k] = u + v;
                fft[i + k + half] = u - v;
                w *= wlen;
            }
        }
    }

    // Map FFT bins to 64 logarithmically spaced frequency bands (20 Hz .. 20 kHz)
    const double minFreq = 20.0;
    const double maxFreq = std::min(20000.0, sampleRate_ * 0.49);
    const double logRatio = std::log(maxFreq / minFreq);
    const float invNorm = 2.0f / (kFftSize * 0.5f); // Hann window coherent gain compensation

    for (int b = 0; b < WA_SPECTRUM_BINS; ++b) {
        const double f0 = minFreq * std::exp(logRatio * (static_cast<double>(b) / WA_SPECTRUM_BINS));
        const double f1 = minFreq * std::exp(logRatio * (static_cast<double>(b + 1) / WA_SPECTRUM_BINS));

        int bin0 = std::clamp(static_cast<int>(std::floor(f0 * kFftSize / sampleRate_)), 1, kFftSize / 2 - 1);
        int bin1 = std::clamp(static_cast<int>(std::ceil(f1 * kFftSize / sampleRate_)), bin0, kFftSize / 2 - 1);

        float maxMag = 0.0f;
        for (int k = bin0; k <= bin1; ++k) {
            float mag = std::abs(fft[k]) * invNorm;
            if (mag > maxMag) maxMag = mag;
        }

        float db = clampf(linearToDb(maxMag), -90.0f, 0.0f);
        // Fast attack, smooth decay for crisp visual display
        if (db > smoothedDb_[b]) {
            smoothedDb_[b] = smoothedDb_[b] * 0.35f + db * 0.65f;
        } else {
            smoothedDb_[b] = smoothedDb_[b] * 0.78f + db * 0.22f;
        }
        outBinsDb[b] = smoothedDb_[b];
    }
}

} // namespace winaudio::dsp
