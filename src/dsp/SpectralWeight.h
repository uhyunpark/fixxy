#pragma once

#include <array>
#include <cmath>
#include <complex>

#include "dsp/BandResponse.h"

/**
    Shared contract for fixxy Spectral's weight-curve feature: up to
    kMaxWeightBands user-drawn bands that will (once the DSP lands) bias the
    node's per-bin detection away from user-picked regions, plus the gate
    curve that fades a band's influence out smoothly beyond a user-set
    [rangeLoHz, rangeHiHz] frequency window.

    SINGLE SOURCE OF TRUTH: this header is shared by SpectralNode (audio
    thread), the editor's curve drawing (message thread), and the Python
    proto (proto/spectral_weight.py) that will derive the actual weight math.
    The three must never diverge — see src/dsp/AGENTS.md's "UI-facing math
    shares a single source" convention (BandResponse.h precedent).

    SINGLE-SOURCE weight math lives here: `gateDb` (the [lo,hi] range gate) and
    `bandWeightDb` / `BandWeightEval` (per-band magnitude, the SAME analog-SVF
    prototype recipe the EQ uses via svfcoeffs / BandResponse.h). SpectralNode's
    audio-thread weight table and the editor's drawn weight curve BOTH consume
    these — never a second, divergent evaluator. Ported from the verified
    proto/spectral_weight.py (band_response_db + gate_db, v2 rule).

    Band type indices reference svfcoeffs::Type (src/dsp/BandResponse.h):
    Bell=0, LowShelf, HighShelf, Tilt, Notch, Bandpass, Lowpass, Highpass.
    Not redefined here to avoid a second ordinal contract that could drift
    from the EQ's.
*/
namespace spectralweight {

inline constexpr int kMaxWeightBands = 8;

/** One weight-curve band. `type` indexes svfcoeffs::Type (BandResponse.h). */
struct WeightBand {
    bool on = false;
    int type = 0;
    double freqHz = 1000.0;
    double gainDb = 0.0;
    double q = 0.7;

    bool operator==(const WeightBand&) const = default;
};

// Gate envelope: fades a band's influence out over kGateRampOct octaves as
// frequency approaches/crosses [lo, hi], reaching kGateFloorDb at the edge
// and beyond. Raised-cosine ramp (no discontinuity in slope at the ramp
// boundary, unlike a linear fade).
inline constexpr double kGateFloorDb = -60.0;
inline constexpr double kGateRampOct = 1.0 / 3.0;

namespace detail {
constexpr double kPi = 3.14159265358979323846;

// Raised-cosine ease, t in [0,1]: 0 at t=0, 1 at t=1, zero slope at both ends.
inline double edge01(double t) {
    return 0.5 * (1.0 - std::cos(kPi * t));
}

// One-sided edge falloff: 0 dB gate for x >= 0 (inside the range), ramping to
// kGateFloorDb over the next kGateRampOct octaves outside it, and pinned at
// kGateFloorDb beyond that.
inline double edgeDb(double x) {
    if (x >= 0.0) return 0.0;
    if (x <= -kGateRampOct) return kGateFloorDb;
    return kGateFloorDb * edge01(-x / kGateRampOct);
}
}  // namespace detail

/** Gate attenuation (dB, <= 0) for frequency f given the user's [lo, hi]
    range window. FROZEN v2 rule (proto/spectral_weight.py gate_db): each edge
    is ACTIVE only when it differs from the full param-range extreme —
        loActive = lo > 20.0,   hiActive = hi < 20000.0
    An INACTIVE edge contributes exactly 0 everywhere (including at DC). So the
    DEFAULT range (lo=20, hi=20000) with bands off gives weight 0 at every bin
    — the gate is fully OFF and the pipeline is bit-identical to the no-weight
    core. An active edge ramps 0 -> kGateFloorDb over kGateRampOct octaves
    outside it (raised cosine); both active edges sum, so a collapsed range
    gates hard. DC (f<=0): 2*kGateFloorDb if the lo edge is active, else 0 —
    only the lo side needs the override (log2(0/lo) = -inf); the hi side's
    log2(hi/0+) = +inf already gives 0 regardless of hiActive. */
inline double gateDb(double f, double lo, double hi) {
    const bool loActive = lo > 20.0;
    const bool hiActive = hi < 20000.0;
    if (f <= 0.0) return loActive ? 2.0 * kGateFloorDb : 0.0;
    double g = 0.0;
    if (loActive) g += detail::edgeDb(std::log2(f / lo));
    if (hiActive) g += detail::edgeDb(std::log2(hi / f));
    return g;
}

// Lowpass/Highpass weight bands have no `slope` field, so they take the
// proto's fixed 12 dB/oct convention: a single 2nd-order section using `q`
// directly (the simplest member of the BandResponse.h cascade family).
inline constexpr double kWeightLphpSlope = 12.0;

/** Precomputes ONE weight band's analog-SVF section recipe(s) so a whole
    curve (many frequencies) evaluates without recomputing the recipe per
    point — the hoist SpectralNode's 513-bin table needs, and the shared
    object the editor draws its curve from. Allocation-free (fixed arrays);
    magnitude is the exact BandResponse.h / svfcoeffs prototype (proto
    band_response_db). */
struct BandWeightEval {
    BandWeightEval() = default;

    BandWeightEval(const WeightBand& band, double fs)
        : fs_(fs), type_(static_cast<svfcoeffs::Type>(band.type)), f0_(band.freqHz) {
        g0_ = std::tan(svfcoeffs::kPi * f0_ / fs_);  // 1-pole unwarped cutoff tan (hoisted)
        if (type_ == svfcoeffs::Type::Lowpass || type_ == svfcoeffs::Type::Highpass) {
            const svfcoeffs::CascadePlan plan =
                svfcoeffs::planLowHighPassCascade(kWeightLphpSlope, band.q);
            numSections_ = plan.numSections;
            hasOnePole_ = plan.hasOnePole;
            for (int i = 0; i < numSections_; ++i)
                recipes_[static_cast<std::size_t>(i)] =
                    svfcoeffs::computeSectionRecipe(type_, f0_, 0.0,
                                                    plan.sectionQ[static_cast<std::size_t>(i)], fs_);
        } else {
            numSections_ = 1;
            hasOnePole_ = false;
            recipes_[0] = svfcoeffs::computeSectionRecipe(type_, f0_, band.gainDb, band.q, fs_);
        }
    }

    /** Band magnitude (dB) given the PRECOMPUTED tangent tanPiFOverFs =
        tan(pi*freqHz/fs). tan(pi*f/fs) is band-independent and bin-static, so a
        many-frequency sweep (SpectralNode's 513-bin table) precomputes it once
        per bin and hoists it out of the per-band loop. freqHz is unused here —
        kept for call-site symmetry with the one-arg overload. Bit-identical to
        db(freqHz). */
    double db(double /*freqHz*/, double tanPiFOverFs) const {
        std::complex<double> H(1.0, 0.0);
        for (int i = 0; i < numSections_; ++i)
            H *= svfcoeffs::evalSection(recipes_[static_cast<std::size_t>(i)], tanPiFOverFs);
        if (hasOnePole_)
            H *= svfcoeffs::evalOnePole(g0_, tanPiFOverFs, type_ == svfcoeffs::Type::Highpass);
        const double mag = std::abs(H);
        return 20.0 * std::log10(mag > 1.0e-300 ? mag : 1.0e-300);
    }

    /** Band magnitude (dB) at freqHz. The UI's one-shot curve path (unchanged).
        Delegates to the precomputed-tangent overload (bit-identical). */
    double db(double freqHz) const {
        return db(freqHz, std::tan(svfcoeffs::kPi * freqHz / fs_));
    }

private:
    double fs_ = 44100.0;
    svfcoeffs::Type type_ = svfcoeffs::Type::Bell;
    double f0_ = 1000.0;
    double g0_ = 0.0;  // tan(pi*f0/fs), hoisted 1-pole cutoff tan
    int numSections_ = 0;
    bool hasOnePole_ = false;
    std::array<svfcoeffs::SectionRecipe, svfcoeffs::kMaxSections> recipes_{};
};

/** One weight band's magnitude (dB) at a single frequency. Convenience
    wrapper over BandWeightEval for one-shot use; for a whole curve build a
    BandWeightEval once and call db() per point (recipe hoisted). */
inline double bandWeightDb(const WeightBand& band, double freqHz, double fs) {
    return BandWeightEval(band, fs).db(freqHz);
}

// UI y-window half-range (dB) for drawing the weight curve.
inline constexpr double kWeightDisplayDb = 24.0;

}  // namespace spectralweight
