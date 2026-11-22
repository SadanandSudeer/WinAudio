#include "Equalizer.h"

namespace winaudio::dsp {

void Equalizer::prepare(double sampleRate) noexcept {
    sampleRate_ = std::max(8000.0, sampleRate);
    reset();
}

void Equalizer::reset() noexcept {
    for (auto& b : bands_) {
        b.reset();
    }
}

void Equalizer::updateParams(const WaEqParams& params) noexcept {
    enabled_ = (params.enabled != 0);
    activeCount_ = 0;

    if (!enabled_) return;

    const auto mode = static_cast<WaEqMode>(params.mode);

    if (mode == WA_EQ_10BAND) {
        constexpr double q10 = 1.41421356; // 1-octave bandwidth
        for (int i = 0; i < 10; ++i) {
            double gain = clampd(params.graphicGainsDb[i], -15.0, 15.0);
            auto c = makeBiquad(WA_FILTER_PEAKING, sampleRate_, kEq10Freqs[i], gain, q10);
            bands_[i].setCoeffs(c);
            if (c.active) activeIndices_[activeCount_++] = i;
        }
    } else if (mode == WA_EQ_31BAND) {
        constexpr double q31 = 4.318; // 1/3-octave bandwidth
        for (int i = 0; i < 31; ++i) {
            double gain = clampd(params.graphicGainsDb[i], -15.0, 15.0);
            auto c = makeBiquad(WA_FILTER_PEAKING, sampleRate_, kEq31Freqs[i], gain, q31);
            bands_[i].setCoeffs(c);
            if (c.active) activeIndices_[activeCount_++] = i;
        }
    } else {
        for (int i = 0; i < WA_MAX_PARAM_BANDS; ++i) {
            const auto& pb = params.paramBands[i];
            if (!pb.enabled) {
                bands_[i].setCoeffs({});
                continue;
            }
            auto ftype = static_cast<WaFilterType>(std::clamp(pb.filterType, 0, 5));
            double gain = clampd(pb.gainDb, -18.0, 18.0);
            double q = clampd(pb.q, 0.3, 12.0);
            auto c = makeBiquad(ftype, sampleRate_, pb.frequencyHz, gain, q);
            bands_[i].setCoeffs(c);
            if (c.active) activeIndices_[activeCount_++] = i;
        }
    }
}

double Equalizer::evalResponseDb(const WaEqParams& params, double sampleRate, double freqHz) noexcept {
    if (!params.enabled) return 0.0;
    double totalDb = 0.0;
    const auto mode = static_cast<WaEqMode>(params.mode);

    if (mode == WA_EQ_10BAND) {
        constexpr double q10 = 1.41421356;
        for (int i = 0; i < 10; ++i) {
            double gain = clampd(params.graphicGainsDb[i], -15.0, 15.0);
            auto c = makeBiquad(WA_FILTER_PEAKING, sampleRate, kEq10Freqs[i], gain, q10);
            totalDb += evalBiquadMagDb(c, sampleRate, freqHz);
        }
    } else if (mode == WA_EQ_31BAND) {
        constexpr double q31 = 4.318;
        for (int i = 0; i < 31; ++i) {
            double gain = clampd(params.graphicGainsDb[i], -15.0, 15.0);
            auto c = makeBiquad(WA_FILTER_PEAKING, sampleRate, kEq31Freqs[i], gain, q31);
            totalDb += evalBiquadMagDb(c, sampleRate, freqHz);
        }
    } else {
        for (int i = 0; i < WA_MAX_PARAM_BANDS; ++i) {
            const auto& pb = params.paramBands[i];
            if (!pb.enabled) continue;
            auto ftype = static_cast<WaFilterType>(std::clamp(pb.filterType, 0, 5));
            double gain = clampd(pb.gainDb, -18.0, 18.0);
            double q = clampd(pb.q, 0.3, 12.0);
            auto c = makeBiquad(ftype, sampleRate, pb.frequencyHz, gain, q);
            totalDb += evalBiquadMagDb(c, sampleRate, freqHz);
        }
    }
    return totalDb;
}

} // namespace winaudio::dsp
