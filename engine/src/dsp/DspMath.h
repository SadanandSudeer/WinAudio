#pragma once

#include "WinAudioABI.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace winaudio::dsp {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr float kMinDb = -96.0f;

inline float dbToLinear(float db) noexcept {
    return std::pow(10.0f, db * 0.05f);
}

inline double dbToLinearD(double db) noexcept {
    return std::pow(10.0, db * 0.05);
}

inline float linearToDb(float lin) noexcept {
    if (lin <= 1e-6f) return kMinDb;
    return std::max(kMinDb, 20.0f * std::log10(lin));
}

inline float clampf(float v, float lo, float hi) noexcept {
    return std::max(lo, std::min(hi, v));
}

inline double clampd(double v, double lo, double hi) noexcept {
    return std::max(lo, std::min(hi, v));
}

// Smooth one-pole exponential parameter smoother (prevents slider zipper noise)
class OnePoleSmoother {
public:
    void prepare(double sampleRate, double timeConstMs, float initialValue = 0.0f) noexcept {
        sampleRate_ = std::max(8000.0, sampleRate);
        setTimeConstantMs(timeConstMs);
        current_ = initialValue;
        target_ = initialValue;
    }

    void setTimeConstantMs(double ms) noexcept {
        double tauSec = std::max(0.0005, ms * 0.001);
        alpha_ = static_cast<float>(1.0 - std::exp(-1.0 / (tauSec * sampleRate_)));
    }

    void setTarget(float target) noexcept { target_ = target; }
    void snap(float value) noexcept { current_ = target_ = value; }
    float current() const noexcept { return current_; }

    inline float next() noexcept {
        current_ += alpha_ * (target_ - current_);
        return current_;
    }

private:
    double sampleRate_ = 48000.0;
    float alpha_ = 0.002f;
    float current_ = 0.0f;
    float target_ = 0.0f;
};

struct BiquadCoeffs {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    bool active = false;
};

// RBJ Audio EQ Cookbook exact double-precision coefficient calculation
inline BiquadCoeffs makeBiquad(WaFilterType type, double sampleRate, double freqHz, double gainDb, double q) noexcept {
    BiquadCoeffs c{};
    sampleRate = std::max(8000.0, sampleRate);
    double nyquist = sampleRate * 0.495;
    freqHz = clampd(freqHz, 10.0, nyquist);
    q = clampd(q, 0.1, 20.0);

    if ((type == WA_FILTER_PEAKING || type == WA_FILTER_LOW_SHELF || type == WA_FILTER_HIGH_SHELF) &&
        std::abs(gainDb) < 0.01) {
        c.active = false;
        return c;
    }

    const double w0 = kTwoPi * freqHz / sampleRate;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double alpha = sinW0 / (2.0 * q);
    const double A = std::pow(10.0, gainDb / 40.0);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (type) {
    case WA_FILTER_PEAKING:
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cosW0;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha / A;
        break;

    case WA_FILTER_LOW_SHELF: {
        const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * (sinW0 / 2.0) * std::sqrt((A + 1.0 / A) * (1.0 / clampd(q, 0.25, 2.0) - 1.0) + 2.0);
        b0 = A * ((A + 1.0) - (A - 1.0) * cosW0 + twoSqrtAAlpha);
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosW0);
        b2 = A * ((A + 1.0) - (A - 1.0) * cosW0 - twoSqrtAAlpha);
        a0 = (A + 1.0) + (A - 1.0) * cosW0 + twoSqrtAAlpha;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosW0);
        a2 = (A + 1.0) + (A - 1.0) * cosW0 - twoSqrtAAlpha;
        break;
    }

    case WA_FILTER_HIGH_SHELF: {
        const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * (sinW0 / 2.0) * std::sqrt((A + 1.0 / A) * (1.0 / clampd(q, 0.25, 2.0) - 1.0) + 2.0);
        b0 = A * ((A + 1.0) + (A - 1.0) * cosW0 + twoSqrtAAlpha);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW0);
        b2 = A * ((A + 1.0) + (A - 1.0) * cosW0 - twoSqrtAAlpha);
        a0 = (A + 1.0) - (A - 1.0) * cosW0 + twoSqrtAAlpha;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosW0);
        a2 = (A + 1.0) - (A - 1.0) * cosW0 - twoSqrtAAlpha;
        break;
    }

    case WA_FILTER_LOW_PASS:
        b0 = (1.0 - cosW0) * 0.5;
        b1 = 1.0 - cosW0;
        b2 = (1.0 - cosW0) * 0.5;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha;
        break;

    case WA_FILTER_HIGH_PASS:
        b0 = (1.0 + cosW0) * 0.5;
        b1 = -(1.0 + cosW0);
        b2 = (1.0 + cosW0) * 0.5;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha;
        break;

    case WA_FILTER_NOTCH:
        b0 = 1.0;
        b1 = -2.0 * cosW0;
        b2 = 1.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW0;
        a2 = 1.0 - alpha;
        break;
    }

    const double invA0 = 1.0 / a0;
    c.b0 = b0 * invA0;
    c.b1 = b1 * invA0;
    c.b2 = b2 * invA0;
    c.a1 = a1 * invA0;
    c.a2 = a2 * invA0;
    c.active = true;
    return c;
}

// Exact analytical magnitude response |H(e^{jw})| in dB for a biquad
inline double evalBiquadMagDb(const BiquadCoeffs& c, double sampleRate, double freqHz) noexcept {
    if (!c.active) return 0.0;
    const double w = kTwoPi * freqHz / sampleRate;
    const double cw = std::cos(w);
    const double sw = std::sin(w);
    const double c2w = std::cos(2.0 * w);
    const double s2w = std::sin(2.0 * w);

    const double numRe = c.b0 + c.b1 * cw + c.b2 * c2w;
    const double numIm = -(c.b1 * sw + c.b2 * s2w);
    const double denRe = 1.0 + c.a1 * cw + c.a2 * c2w;
    const double denIm = -(c.a1 * sw + c.a2 * s2w);

    const double magSq = (numRe * numRe + numIm * numIm) / std::max(1e-24, denRe * denRe + denIm * denIm);
    return 10.0 * std::log10(std::max(1e-12, magSq));
}

// Stereo Direct Form II Transposed Biquad with double-precision internal state
class StereoBiquad {
public:
    void setCoeffs(const BiquadCoeffs& c) noexcept { coeffs_ = c; }
    const BiquadCoeffs& coeffs() const noexcept { return coeffs_; }
    bool active() const noexcept { return coeffs_.active; }

    void reset() noexcept {
        z1L_ = z2L_ = 0.0;
        z1R_ = z2R_ = 0.0;
    }

    inline void process(double& l, double& r) noexcept {
        if (!coeffs_.active) return;

        const double outL = coeffs_.b0 * l + z1L_;
        z1L_ = coeffs_.b1 * l - coeffs_.a1 * outL + z2L_;
        z2L_ = coeffs_.b2 * l - coeffs_.a2 * outL;
        l = outL;

        const double outR = coeffs_.b0 * r + z1R_;
        z1R_ = coeffs_.b1 * r - coeffs_.a1 * outR + z2R_;
        z2R_ = coeffs_.b2 * r - coeffs_.a2 * outR;
        r = outR;
    }

private:
    BiquadCoeffs coeffs_{};
    double z1L_ = 0.0, z2L_ = 0.0;
    double z1R_ = 0.0, z2R_ = 0.0;
};

} // namespace winaudio::dsp
