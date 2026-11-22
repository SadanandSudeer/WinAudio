#include "Maximizer.h"

namespace winaudio::dsp {

void Maximizer::prepare(double sampleRate) noexcept {
    sampleRate_ = std::max(8000.0, sampleRate);
    boostGain_.prepare(sampleRate_, 15.0, 1.0f);

    // 3.0 ms lookahead for zero-overshoot brickwall peak protection
    lookaheadSamples_ = std::clamp(static_cast<int>(std::round(sampleRate_ * 0.003)), 16, 1024);
    delayL_.assign(lookaheadSamples_, 0.0);
    delayR_.assign(lookaheadSamples_, 0.0);
    dequeCap_ = lookaheadSamples_ + 4;
    dequeIdx_.assign(dequeCap_, 0);
    dequeVal_.assign(dequeCap_, 0.0);

    fastReleaseCoeff_ = std::exp(-1.0 / (0.035 * sampleRate_));
    slowReleaseCoeff_ = std::exp(-1.0 / (0.180 * sampleRate_));
    reset();
}

void Maximizer::reset() noexcept {
    std::fill(delayL_.begin(), delayL_.end(), 0.0);
    std::fill(delayR_.begin(), delayR_.end(), 0.0);
    dequeHead_ = dequeTail_ = 0;
    sampleCounter_ = 0;
    writeIdx_ = 0;
    gainEnvelope_ = 1.0;
    slowEnvelope_ = 1.0;
    gainReductionDb_ = 0.0f;
}

void Maximizer::updateParams(const WaDspParams& params) noexcept {
    enabled_ = (params.maximizerEnabled != 0);
    float boostLin = enabled_ ? dbToLinear(clampf(params.maximizerBoostDb, 0.0f, 18.0f)) : 1.0f;
    boostGain_.setTarget(boostLin);

    double ceilDb = clampd(params.maximizerCeilingDb, -6.0, -0.1);
    ceilingLinear_ = dbToLinearD(ceilDb);
    softClipAmount_ = clampd(params.maximizerSoftClip, 0.0, 1.0);

    double relMs = clampd(params.maximizerReleaseMs, 20.0, 400.0);
    fastReleaseCoeff_ = std::exp(-1.0 / (std::max(0.015, relMs * 0.0004) * sampleRate_));
    slowReleaseCoeff_ = std::exp(-1.0 / (relMs * 0.001 * sampleRate_));
}

void Maximizer::processSample(double& l, double& r) noexcept {
    const double boost = static_cast<double>(boostGain_.next());

    // Apply controlled boost before lookahead peak detection
    double inL = l * boost;
    double inR = r * boost;

    // Optional soft-saturation warmth knee when driving hard
    if (enabled_ && softClipAmount_ > 1e-4) {
        const double knee = ceilingLinear_ * 0.85;
        auto softKnee = [&](double x) noexcept {
            const double ax = std::abs(x);
            if (ax <= knee) return x;
            const double sign = (x >= 0.0) ? 1.0 : -1.0;
            const double over = (ax - knee) / std::max(1e-6, ceilingLinear_);
            const double shaped = knee + (ceilingLinear_ * 0.25) * std::tanh(over * 2.5);
            return sign * (ax + softClipAmount_ * (shaped - ax));
        };
        inL = softKnee(inL);
        inR = softKnee(inR);
    }

    // Read delayed sample from lookahead buffer
    const double delayedL = delayL_[writeIdx_];
    const double delayedR = delayR_[writeIdx_];

    // Write incoming boosted sample into lookahead ring
    delayL_[writeIdx_] = inL;
    delayR_[writeIdx_] = inR;
    const double curPeak = std::max(std::abs(inL), std::abs(inR));

    // O(1) amortized monotonic deque sliding-window maximum over lookaheadSamples_
    const uint64_t idx = sampleCounter_++;
    while (dequeHead_ != dequeTail_) {
        int back = (dequeTail_ - 1 + dequeCap_) % dequeCap_;
        if (dequeVal_[back] <= curPeak) {
            dequeTail_ = back;
        } else {
            break;
        }
    }
    dequeIdx_[dequeTail_] = idx;
    dequeVal_[dequeTail_] = curPeak;
    dequeTail_ = (dequeTail_ + 1) % dequeCap_;

    // Pop expired entries outside [idx - lookaheadSamples_ + 1 .. idx]
    while (dequeHead_ != dequeTail_ && idx >= static_cast<uint64_t>(lookaheadSamples_) &&
           dequeIdx_[dequeHead_] <= idx - static_cast<uint64_t>(lookaheadSamples_)) {
        dequeHead_ = (dequeHead_ + 1) % dequeCap_;
    }

    writeIdx_++;
    if (writeIdx_ >= lookaheadSamples_) writeIdx_ = 0;

    if (!enabled_) {
        // Still apply gentle safety ceiling at 0.995 when disabled to prevent digital clipping from EQ boosts
        const double peak = std::max(std::abs(delayedL), std::abs(delayedR));
        double safeGain = (peak > 0.995) ? (0.995 / peak) : 1.0;
        gainEnvelope_ = std::min(safeGain, gainEnvelope_ + (1.0 - fastReleaseCoeff_) * (1.0 - gainEnvelope_));
        l = clampd(delayedL * gainEnvelope_, -0.999, 0.999);
        r = clampd(delayedR * gainEnvelope_, -0.999, 0.999);
        gainReductionDb_ = linearToDb(static_cast<float>(gainEnvelope_));
        return;
    }

    const double windowPeak = (dequeHead_ != dequeTail_) ? dequeVal_[dequeHead_] : 0.0;

    const double requiredGain = (windowPeak > ceilingLinear_) ? (ceilingLinear_ / windowPeak) : 1.0;

    // Instantaneous downward attack on lookahead peak, dual-stage smooth upward release
    if (requiredGain < gainEnvelope_) {
        gainEnvelope_ = requiredGain;
    } else {
        gainEnvelope_ = requiredGain + fastReleaseCoeff_ * (gainEnvelope_ - requiredGain);
    }

    if (requiredGain < slowEnvelope_) {
        slowEnvelope_ = requiredGain + 0.85 * (slowEnvelope_ - requiredGain);
    } else {
        slowEnvelope_ = requiredGain + slowReleaseCoeff_ * (slowEnvelope_ - requiredGain);
    }

    const double finalGain = std::min(gainEnvelope_, slowEnvelope_);

    // Hard brickwall safety clamp guarantees peaks NEVER exceed ceilingLinear_
    l = clampd(delayedL * finalGain, -ceilingLinear_, ceilingLinear_);
    r = clampd(delayedR * finalGain, -ceilingLinear_, ceilingLinear_);

    gainReductionDb_ = linearToDb(static_cast<float>(finalGain));
}

} // namespace winaudio::dsp
