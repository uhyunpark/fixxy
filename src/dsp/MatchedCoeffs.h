#pragma once

#include <cmath>
#include <complex>

#include "dsp/BandResponse.h"

/**
    Ivantsov "phi-mapped" matched-IIR coefficients + normal-form state-space
    realization. This is the SINGLE source of the digital biquad (b0,b1,b2,a1,a2)
    for the six "matched" band types — Bell, LowShelf, HighShelf, Tilt, Notch,
    Bandpass — consumed by BOTH the audio kernel (SvfBandNode) and the UI
    response evaluator (bandMagnitudeDb below). Low/High-pass are NOT here: they
    stay on BandResponse.h's TPT-SVF Butterworth cascade (a matched per-section
    LP cascade compounds up to 4.35 dB of Nyquist residual at 96 dB/oct — proto
    test 7 carve-out).

    WHY a different filter design at all (the DSP "why"): the incumbent TPT-SVF
    uses the bilinear transform, which squeezes the whole frequency axis toward
    Nyquist ("cramping") — a bell at 15 kHz lands flat and narrow of its analog
    ideal. Ivantsov's phi() pole map plus a mandatory frequency-placement
    correction fixes that: the digital magnitude tracks the analog prototype to
    ~0.5 dB in the musical core (vs the BLT's several dB near the top octave),
    and is 3.5-10x better than the BLT on the resonant shelves.

    WHY the "normal form" realization (not a plain biquad): parameter smoothing
    recomputes coefficients every block while the filter's state is live. A
    direct-form biquad stores delay memory (z1,z2) that is only valid for the
    exact coefficients that produced it — swap coefficients mid-flight and you
    inject a transient the new filter was never designed to carry (audible zipper
    / click at high Q/gain). The normal form's 2x2 state matrix A=[[re,im],
    [-im,re]] has spectral norm exactly equal to the pole radius (< 1), the
    theoretical best conditioning, so its state stays physically meaningful when
    coefficients change. That is what lets one topology serve every matched type
    AND survive fast modulation. (Proto test 6 confirmed peak/zipper parity with
    the smooth TPT-SVF reference; proto test 5 confirmed the realization
    reconstructs H(z) to 1e-13.)

    Ground truth: proto/matched_kernel.py (VERIFIED). Formulas ported verbatim —
    the phi/nu/kappa machinery, the sigma constant, the corrected-omega frequency
    placement, Ivantsov Table 3.1, tilt = (1/A)*high_shelf(g=A^2), and the
    complex/real normal-form branches. Do not re-derive here; edit the proto,
    re-verify, then re-port.
*/
namespace matched {

constexpr double kPi = 3.14159265358979323846;

// Optimal shape constant. Ivantsov's phi() pole map has a free parameter sigma
// (paper range [2, pi*sqrt(2/3)]); larger sigma bends the pole placement more.
// This exact value is the argmin of the worst-case error across the accuracy
// grid, constrained so the benchmark bell set stays under 0.5 dB (proto test 2,
// grid point 38 of linspace(0.05, pi-0.02, 60)). The proto's gates were
// verified AT this value — treat it as load-bearing, not a tunable.
constexpr double kSigma = 2.0283139124815617;

// --- Ivantsov phi machinery (proto: phi/nu/kappa/phi1/phi2) ---------------

inline double phi(double x, double sigma) {
    const double r = std::sqrt(x * x + sigma * sigma);
    return (kPi - r) / (kPi + r);
}

inline double nuFn(double x, double y, double sigma) {
    const double x2 = x * x;
    const double s2 = sigma * sigma;
    return std::sqrt(x2 * x2 + 2.0 * x2 * s2 * (2.0 * y * y - 1.0) + s2 * s2);
}

inline double kappaFn(double x, double y, double sigma) {
    return x * x * (2.0 * y * y - 1.0) + sigma * sigma;
}

inline double phi1(double x, double y, double sigma) {
    const double nu = nuFn(x, y, sigma);
    const double kappa = kappaFn(x, y, sigma);
    // nu+kappa is analytically >= 0 but roundoff can push it slightly negative
    // at y=0 with x>sigma (the notch-zero case); clamp before the sqrt.
    const double s = nu + kappa;
    const double root = std::sqrt(s > 0.0 ? s : 0.0);
    const double denom = kPi * kPi + kPi * std::sqrt(2.0) * root + nu;
    return (2.0 * kPi * kPi - 2.0 * nu) / denom;
}

inline double phi2(double x, double y, double sigma) {
    const double nu = nuFn(x, y, sigma);
    const double kappa = kappaFn(x, y, sigma);
    const double s = nu + kappa;
    const double root = std::sqrt(s > 0.0 ? s : 0.0);
    const double denom = kPi * kPi + kPi * std::sqrt(2.0) * root + nu;
    return (kPi * kPi - kPi * std::sqrt(2.0) * root + nu) / denom;
}

// Mandatory frequency-placement correction (proto: corrected_omega). The phi
// map does not land the resonance exactly at fs/x; this inversion of the paper's
// F(omega_c)=fc places it exactly, and WITHOUT it the design misses its gates by
// 2-50 dB. For an analog resonance at/above Nyquist (x <= 2) it cannot be
// placed, so fall back to the uncorrected x (graceful, phi -> -1). Reached only
// via extreme shelf gain shifts, never on the accuracy grid.
inline double correctedOmega(double x, double sigma) {
    if (x <= 2.001)
        return x;
    const double c = kPi / std::tan(kPi / x);
    return std::sqrt(sigma * sigma + c * c);
}

// --- Digital biquad -------------------------------------------------------

struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;  // numerator
    double a1 = 0.0, a2 = 0.0;            // denominator (a0 == 1)
};

/** True for the six types realized by the Ivantsov+normal-form kernel. Low/High-
    pass (which cascade Butterworth sections) stay on the TPT-SVF path. */
inline bool isMatchedType(svfcoeffs::Type t) {
    return t != svfcoeffs::Type::Lowpass && t != svfcoeffs::Type::Highpass;
}

/** Ivantsov Table 3.1 second-order design -> digital (b0,b1,b2,a1,a2). Ported
    verbatim from proto/matched_kernel.py ivantsov_2nd + ivantsov_tilt. gainDb is
    used by bell/shelf/tilt only (ignored for notch/bandpass). */
inline Biquad computeBiquad(svfcoeffs::Type type, double fs, double f0, double Q,
                            double gainDb, double sigma = kSigma) {
    using T = svfcoeffs::Type;

    const double omega = fs / f0;              // Ivantsov convention: larger = LOWER f0
    const double zeta = 1.0 / (2.0 * Q);
    const double G = std::pow(10.0, gainDb / 20.0);  // linear gain (== A^2)
    const double oc = correctedOmega(omega, sigma);

    // tilt = (1/A) * high_shelf(g=A^2): identical high-shelf coefficients, then
    // numerator scaled by 1/A. Feeding the SAME gainDb yields g=A^2 for free.
    const bool isTilt = (type == T::Tilt);
    const double tiltScale = isTilt ? 1.0 / std::pow(10.0, gainDb / 40.0) : 1.0;
    const T design = isTilt ? T::HighShelf : type;

    double alpha1 = 0.0, alpha2 = 0.0, beta1 = 0.0, beta2 = 0.0, Ggain = 1.0;
    switch (design) {
    case T::Bell: {
        const double sg = std::sqrt(G);
        alpha1 = phi1(oc, zeta * sg, sigma);  alpha2 = phi2(oc, zeta * sg, sigma);
        beta1  = phi1(oc, zeta / sg, sigma);  beta2  = phi2(oc, zeta / sg, sigma);
        Ggain = 1.0 / (1.0 + alpha1 + alpha2);
        break;
    }
    case T::LowShelf: {
        const double oz = correctedOmega(omega * std::pow(G, -0.25), sigma);  // zeros at fc/g^(1/4)
        const double op = correctedOmega(omega * std::pow(G,  0.25), sigma);  // poles at fc*g^(1/4)
        alpha1 = phi1(oz, zeta, sigma);  alpha2 = phi2(oz, zeta, sigma);
        beta1  = phi1(op, zeta, sigma);  beta2  = phi2(op, zeta, sigma);
        Ggain = G / (1.0 + alpha1 + alpha2);
        break;
    }
    case T::HighShelf: {  // also the tilt design branch
        const double oz = correctedOmega(omega * std::pow(G,  0.25), sigma);
        const double op = correctedOmega(omega * std::pow(G, -0.25), sigma);
        alpha1 = phi1(oz, zeta, sigma);  alpha2 = phi2(oz, zeta, sigma);
        beta1  = phi1(op, zeta, sigma);  beta2  = phi2(op, zeta, sigma);
        Ggain = 1.0 / (1.0 + alpha1 + alpha2);
        break;
    }
    case T::Notch: {
        alpha1 = phi1(oc, 0.0, sigma);   alpha2 = phi2(oc, 0.0, sigma);
        beta1  = phi1(oc, zeta, sigma);  beta2  = phi2(oc, zeta, sigma);
        Ggain = 1.0 / (1.0 + alpha1 + alpha2);
        break;
    }
    case T::Bandpass: {
        const double p0 = phi(0.0, sigma), pinf = -1.0;
        alpha1 = p0 + pinf;              alpha2 = p0 * pinf;
        beta1  = phi1(oc, zeta, sigma);  beta2  = phi2(oc, zeta, sigma);
        Ggain = (oc / (2.0 * kPi)) * (2.0 * zeta / (2.0 + alpha1));
        break;
    }
    case T::Tilt:      // handled via design == HighShelf above
    case T::Lowpass:   // not matched types — never reach here (see isMatchedType)
    case T::Highpass:
        break;
    }

    const double b0scale = Ggain * (1.0 + beta1 + beta2);
    Biquad q;
    q.b0 = b0scale * tiltScale;
    q.b1 = b0scale * alpha1 * tiltScale;
    q.b2 = b0scale * alpha2 * tiltScale;
    q.a1 = beta1;
    q.a2 = beta2;
    return q;
}

/** Exact digital magnitude of the biquad in dB — |H(e^jw)| from the SAME
    coefficients the kernel runs. This is what the UI must draw for matched
    types (truth in display), not the analog ideal. */
inline std::complex<double> biquadResponse(const Biquad& q, double w) {
    const std::complex<double> z1 = std::exp(std::complex<double>(0.0, -w));
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> num = q.b0 + q.b1 * z1 + q.b2 * z2;
    const std::complex<double> den = 1.0 + q.a1 * z1 + q.a2 * z2;
    return num / den;
}

inline double biquadMagnitudeDb(const Biquad& q, double fs, double f) {
    const double w = 2.0 * kPi * f / fs;
    const double m = std::abs(biquadResponse(q, w));
    return 20.0 * std::log10(m > 1.0e-300 ? m : 1.0e-300);
}

// --- Normal-form state-space realization (proto: normal_form_*) -----------

/** Realization of one biquad as a 2-state normal form. Complex-pole branch
    (a1^2 < 4a2): A = [[re,im],[-im,re]]. Real-pole branch (a1^2 >= 4a2):
    diagonal poles l1,l2 in parallel. d is the direct feedthrough, (c1,c2) the
    output mixing / residues. */
struct NormalForm {
    bool isComplex = true;
    double p1 = 0.0, p2 = 0.0;  // complex: (re, im);  real: (l1, l2)
    double c1 = 0.0, c2 = 0.0, d = 0.0;
};

constexpr double kRepeatedPoleEps = 1e-9;

inline NormalForm normalForm(const Biquad& q) {
    const double b0 = q.b0, b1 = q.b1, b2 = q.b2;
    double a1 = q.a1, a2 = q.a2;

    double disc = a1 * a1 - 4.0 * a2;
    // Perturb a2 fractionally on the (measure-zero) exact-repeated-pole boundary
    // to avoid l1==l2 division-by-zero in the real branch's residue formula
    // (Jordan-block hazard). Not hit by musical Q/gain ranges — defensive only.
    if (std::abs(disc) < kRepeatedPoleEps) {
        a2 = (a2 != 0.0) ? a2 * (1.0 - 1e-6) : a2 - 1e-9;
        disc = a1 * a1 - 4.0 * a2;
    }

    NormalForm nf;
    if (disc < 0.0) {
        nf.isComplex = true;
        const double re = -a1 / 2.0;
        const double im = std::sqrt(a2 - a1 * a1 / 4.0);
        nf.p1 = re;
        nf.p2 = im;
        nf.d = b0;
        nf.c1 = b1 - b0 * a1;
        nf.c2 = (b0 * a2 - b2 - nf.c1 * re) / im;
    } else {
        nf.isComplex = false;
        const double ds = std::sqrt(disc);
        const double l1 = (-a1 + ds) / 2.0;
        const double l2 = (-a1 - ds) / 2.0;
        nf.p1 = l1;
        nf.p2 = l2;
        nf.c1 = (b0 * l1 * l1 + b1 * l1 + b2) / (l1 * (l1 - l2));
        nf.c2 = (b0 * l2 * l2 + b1 * l2 + b2) / (l2 * (l2 - l1));
        nf.d = b0 - nf.c1 - nf.c2;
    }
    return nf;
}

/** Complex frequency response of the normal-form realization (pure linear
    algebra, no simulation) — equals biquadResponse() for the source biquad.
    Used only in tests. */
inline std::complex<double> normalFormResponse(const NormalForm& nf, double w) {
    const std::complex<double> z = std::exp(std::complex<double>(0.0, w));
    if (nf.isComplex) {
        const std::complex<double> zc = z - nf.p1;  // z - re
        const std::complex<double> det = zc * zc + nf.p2 * nf.p2;
        const std::complex<double> num = nf.c1 * zc - nf.c2 * nf.p2;
        return nf.d + num / det;
    }
    return nf.d + nf.c1 * z / (z - nf.p1) + nf.c2 * z / (z - nf.p2);
}

// --- Unified band response for the UI curve -------------------------------

/** Full band magnitude at a display frequency, in dB. Matched types are drawn
    from the exact digital biquad the kernel runs; Low/High-pass fall through to
    the TPT-SVF analog-cascade evaluator (BandResponse.h). This is the ONE curve
    entry the UI and the offline cross-check both call. */
inline double bandMagnitudeDb(svfcoeffs::Type type, double f0, double q, double gainDb,
                              double slope, double fs, double f) {
    if (isMatchedType(type)) {
        const Biquad bq = computeBiquad(type, fs, f0, q, gainDb);
        return biquadMagnitudeDb(bq, fs, f);
    }
    return svfcoeffs::magnitudeDb(type, f0, q, gainDb, slope, fs, f);
}

}  // namespace matched
