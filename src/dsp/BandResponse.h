#pragma once

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>

/**
    Shared coefficient recipe + analytic frequency-response evaluator for
    SvfBandNode-class filters. JUCE-free, header-only.

    WHY THIS FILE EXISTS: the UI's response curve and the audio-thread kernel
    (SvfBandNode) must never be able to drift apart — a curve that says "+12 dB
    at 1 kHz" while the actual filter does something else is worse than no
    curve at all. So the (g, k, m0, m1, m2) per-type recipe and the Butterworth
    pole-angle allocation for slope cascades live HERE, in exactly one place.
    SvfBandNode::computeSection() calls computeSectionRecipe() below instead of
    duplicating the switch statement; this file's magnitudeDb() evaluates the
    identical analog SVF prototype the digital TPT form realizes.

    Method (exact, not an approximation, for the bilinear/TPT topology):
      - Normalized prewarped frequency at a given section: W(f) = tan(pi*f/fs) / g
        where g is THAT SECTION's warped cutoff tan (e.g. tan*sqrt(A) for shelves).
      - Analog SVF prototype at s = jW:
            D  = (1 - W^2) + j*k*W
            BP = jW / D
            LP = 1 / D
            H  = m0 + m1*BP + m2*LP
      - Low/high-pass slope cascades: product of per-section complex responses,
        each section built from computeSectionRecipe() with the cascade's
        per-section Q (from planLowHighPassCascade), plus one 1st-order TPT
        section when the order is odd:
            LP1 = 1 / (1 + jW),   HP1 = jW / (1 + jW),   W = tan(pi*f/fs)/tan(pi*f0/fs)

    Ground truth: proto/multiband_eq.py svf_coeffs(). Verified against scipy to
    << 0.1 dB (see test/band_response_test.cpp for the online cross-check
    against the actual SvfBandNode kernel via impulse response + FFT).
*/
namespace svfcoeffs {

constexpr double kPi = 3.14159265358979323846;

enum class Type { Bell = 0, LowShelf, HighShelf, Tilt, Notch, Bandpass, Lowpass, Highpass };

/** Analog SVF-prototype recipe for one 2nd-order section. g is the section's
    OWN warped cutoff tan (already includes any shelf/tilt sqrt(A) shift), so
    downstream consumers must compute W(f) = tan(pi*f/fs) / g, never re-derive
    it from f0 directly. */
struct SectionRecipe {
    double g = 0.0, k = 0.0, m0 = 0.0, m1 = 0.0, m2 = 0.0;
};

/** One SVF section's (g, k, m0, m1, m2) for a given type. This is the SINGLE
    place that recipe is defined — SvfBandNode::computeSection derives its
    digital (a1, a2, a3) from this, and BandResponse's magnitudeDb() evaluates
    the analog prototype directly from this. Ground truth: proto/multiband_eq.py
    svf_coeffs(). */
inline SectionRecipe computeSectionRecipe(Type type, double f0, double gainDb, double q,
                                           double fs) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double tanf = std::tan(kPi * f0 / fs);
    SectionRecipe r;

    switch (type) {
    case Type::Bell:
        r.g = tanf;              r.k = 1.0 / (q * A);
        r.m0 = 1.0;              r.m1 = r.k * (A * A - 1.0);       r.m2 = 0.0;            break;
    case Type::LowShelf:
        r.g = tanf / std::sqrt(A); r.k = 1.0 / q;
        r.m0 = 1.0;              r.m1 = r.k * (A - 1.0);           r.m2 = A * A - 1.0;    break;
    case Type::HighShelf:
        r.g = tanf * std::sqrt(A); r.k = 1.0 / q;
        r.m0 = A * A;            r.m1 = r.k * (1.0 - A) * A;       r.m2 = 1.0 - A * A;    break;
    case Type::Tilt:
        r.g = tanf * std::sqrt(A); r.k = 1.0 / q;
        r.m0 = A;                r.m1 = r.k * (1.0 - A);           r.m2 = 1.0 / A - A;    break;
    case Type::Notch:
        r.g = tanf;              r.k = 1.0 / q;
        r.m0 = 1.0;              r.m1 = -r.k;                      r.m2 = 0.0;            break;
    case Type::Bandpass:
        r.g = tanf;              r.k = 1.0 / q;
        r.m0 = 0.0;              r.m1 = r.k;                       r.m2 = 0.0;            break;
    case Type::Lowpass:
        r.g = tanf;              r.k = 1.0 / q;
        r.m0 = 0.0;              r.m1 = 0.0;                       r.m2 = 1.0;            break;
    case Type::Highpass:
        r.g = tanf;              r.k = 1.0 / q;
        r.m0 = 1.0;              r.m1 = -r.k;                      r.m2 = -1.0;           break;
    }
    return r;
}

constexpr int kMaxSections = 8;  // slope 96 dB/oct -> N=16 -> floor(N/2) = 8.

/** Per-section Q allocation for a low/high-pass Butterworth slope cascade.
    SIN form of the pole angle (cos form is wrong for odd N, verified vs
    scipy — see proto). The Q knob scales only the LAST section; N=1 has no
    2nd-order sections and ignores Q by construction. Single source of truth
    for both SvfBandNode::updateCoeffs and BandResponse's cascade evaluator. */
struct CascadePlan {
    int numSections = 0;
    std::array<double, kMaxSections> sectionQ{};
    bool hasOnePole = false;
};

inline CascadePlan planLowHighPassCascade(double slope, double q) {
    CascadePlan plan;
    int N = static_cast<int>(std::lround(slope / 6.0));
    if (N < 1) N = 1;
    if (N > 2 * kMaxSections) N = 2 * kMaxSections;
    const int nPairs = N / 2;
    plan.numSections = nPairs;

    for (int i = 1; i <= nPairs; ++i) {
        const double kButter = 2.0 * std::sin((2.0 * i - 1.0) * kPi / (2.0 * N));
        double qSec = 1.0 / kButter;
        if (i == nPairs) {
            const double kLast = kButter * (0.7071 / q);
            qSec = 1.0 / kLast;
        }
        plan.sectionQ[static_cast<std::size_t>(i - 1)] = qSec;
    }
    plan.hasOnePole = (N % 2 == 1);
    return plan;
}

/** Complex analog-prototype response of one 2nd-order section at frequency f. */
inline std::complex<double> evalSection(const SectionRecipe& r, double fs, double f) {
    const double W = std::tan(kPi * f / fs) / r.g;
    const std::complex<double> jW(0.0, W);
    const std::complex<double> D(1.0 - W * W, r.k * W);
    const std::complex<double> BP = jW / D;
    const std::complex<double> LP = 1.0 / D;
    return r.m0 + r.m1 * BP + r.m2 * LP;
}

/** Complex response of the odd-order 1st-order TPT section (low/high-pass
    cascades only). g0 = tan(pi*f0/fs) is the UNWARPED cutoff tan (the 1-pole
    section carries no shelf/tilt warp). */
inline std::complex<double> evalOnePole(double f0, double fs, double f, bool highpass) {
    const double g0 = std::tan(kPi * f0 / fs);
    const double W = std::tan(kPi * f / fs) / g0;
    const std::complex<double> jW(0.0, W);
    const std::complex<double> D = 1.0 + jW;
    return highpass ? (jW / D) : (1.0 / D);
}

/** Full band response at a display frequency f, in dB. Mirrors
    SvfBandNode::updateCoeffs()'s section/one-pole assembly exactly: bell/
    shelf/tilt/notch/bandpass use a single section; low/high-pass cascade
    planLowHighPassCascade()'s sections plus an optional 1st-order section. */
inline double magnitudeDb(Type type, double f0, double q, double gainDb, double slope,
                           double fs, double f) {
    std::complex<double> H(1.0, 0.0);

    if (type == Type::Lowpass || type == Type::Highpass) {
        const CascadePlan plan = planLowHighPassCascade(slope, q);
        for (int i = 0; i < plan.numSections; ++i) {
            const SectionRecipe r =
                computeSectionRecipe(type, f0, 0.0, plan.sectionQ[static_cast<std::size_t>(i)], fs);
            H *= evalSection(r, fs, f);
        }
        if (plan.hasOnePole)
            H *= evalOnePole(f0, fs, f, type == Type::Highpass);
    } else {
        const SectionRecipe r = computeSectionRecipe(type, f0, gainDb, q, fs);
        H = evalSection(r, fs, f);
    }

    const double mag = std::abs(H);
    // Guard log10(0) for the (theoretical, not hit at display frequencies)
    // degenerate case.
    return 20.0 * std::log10(mag > 1.0e-300 ? mag : 1.0e-300);
}

}  // namespace svfcoeffs
