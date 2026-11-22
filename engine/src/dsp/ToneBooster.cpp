#include "ToneBooster.h"

namespace winaudio::dsp {

void ToneBooster::prepare(double sampleRate) noexcept {
    sampleRate_ = std::max(8000.0, sampleRate);
    exciterMix_.prepare(sampleRate_, 20.0, 0.0f);

    exciterInLp_.setCoeffs(makeBiquad(WA_FILTER_LOW_PASS, sampleRate_, 120.0, 0.0, 0.707));
    exciterInHp_.setCoeffs(makeBiquad(WA_FILTER_HIGH_PASS, sampleRate_, 35.0, 0.0, 0.707));
    exciterOutHp_.setCoeffs(makeBiquad(WA_FILTER_HIGH_PASS, sampleRate_, 95.0, 0.0, 0.707));
    exciterOutLp_.setCoeffs(makeBiquad(WA_FILTER_LOW_PASS, sampleRate_, 380.0, 0.0, 0.707));
    reset();
}

void ToneBooster::reset() noexcept {
    bassShelf_.reset();
    trebleShelf_.reset();
    clarityBell_.reset();
    exciterInLp_.reset();
    exciterInHp_.reset();
    exciterOutHp_.reset();
    exciterOutLp_.reset();
}

void ToneBooster::updateParams(const WaDspParams& params) noexcept {
    bassEnabled_ = (params.bassEnabled != 0);
    trebleEnabled_ = (params.trebleEnabled != 0);

    if (bassEnabled_) {
        double freq = clampd(params.bassFreqHz, 60.0, 150.0);
        double boost = clampd(params.bassBoostDb, 0.0, 15.0);
        bassShelf_.setCoeffs(makeBiquad(WA_FILTER_LOW_SHELF, sampleRate_, freq, boost, 0.75));
        exciterMix_.setTarget(clampf(params.bassExciterMix, 0.0f, 1.0f));
    } else {
        bassShelf_.setCoeffs({});
        exciterMix_.setTarget(0.0f);
    }

    if (trebleEnabled_) {
        double freq = clampd(params.trebleFreqHz, 3500.0, 10000.0);
        double boost = clampd(params.trebleBoostDb, 0.0, 15.0);
        double clarity = clampd(params.clarityBoostDb, 0.0, 8.0);
        trebleShelf_.setCoeffs(makeBiquad(WA_FILTER_HIGH_SHELF, sampleRate_, freq, boost, 0.75));
        clarityBell_.setCoeffs(makeBiquad(WA_FILTER_PEAKING, sampleRate_, 3100.0, clarity, 0.9));
    } else {
        trebleShelf_.setCoeffs({});
        clarityBell_.setCoeffs({});
    }
}

void ToneBooster::processSample(double& l, double& r) noexcept {
    const float exMix = exciterMix_.next();

    if (bassEnabled_ || exMix > 1e-4f) {
        // Psychoacoustic sub-harmonic exciter generates 2nd & 3rd harmonics of bass fundamentals
        // so small laptop/desktop speakers perceive deep bass punch below their physical roll-off.
        if (exMix > 1e-4f) {
            double subL = l;
            double subR = r;
            exciterInLp_.process(subL, subR);
            exciterInHp_.process(subL, subR);

            // Asymmetric soft waveshaper produces both even (2nd) and odd (3rd) warm harmonics
            auto shape = [](double x) noexcept {
                double driven = x * 2.8;
                double s = std::tanh(driven) + 0.18 * (driven * driven) / (1.0 + std::abs(driven));
                return s * 0.55;
            };

            subL = shape(subL);
            subR = shape(subR);
            exciterOutHp_.process(subL, subR);
            exciterOutLp_.process(subL, subR);

            l += subL * static_cast<double>(exMix) * 0.85;
            r += subR * static_cast<double>(exMix) * 0.85;
        }

        if (bassEnabled_) {
            bassShelf_.process(l, r);
        }
    }

    if (trebleEnabled_) {
        clarityBell_.process(l, r);
        trebleShelf_.process(l, r);
    }
}

double ToneBooster::evalResponseDb(const WaDspParams& params, double sampleRate, double freqHz) noexcept {
    double total = 0.0;
    if (params.bassEnabled) {
        auto c = makeBiquad(WA_FILTER_LOW_SHELF, sampleRate, clampd(params.bassFreqHz, 60.0, 150.0), clampd(params.bassBoostDb, 0.0, 15.0), 0.75);
        total += evalBiquadMagDb(c, sampleRate, freqHz);
    }
    if (params.trebleEnabled) {
        auto c1 = makeBiquad(WA_FILTER_HIGH_SHELF, sampleRate, clampd(params.trebleFreqHz, 3500.0, 10000.0), clampd(params.trebleBoostDb, 0.0, 15.0), 0.75);
        auto c2 = makeBiquad(WA_FILTER_PEAKING, sampleRate, 3100.0, clampd(params.clarityBoostDb, 0.0, 8.0), 0.9);
        total += evalBiquadMagDb(c1, sampleRate, freqHz);
        total += evalBiquadMagDb(c2, sampleRate, freqHz);
    }
    return total;
}

} // namespace winaudio::dsp
