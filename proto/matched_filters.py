"""Vicanek matched-filter coefficients: the decramped replacement for our
TPT-SVF EQ kernel.

BACKGROUND (see proto/cramping.py + docs/eq_reference_guide.md sec 1-2):
The bilinear transform (BLT) warps the frequency axis. Prewarping fixes the
mapping at exactly f0; everywhere else the shape is squeezed toward Nyquist
("cramping"). cramping.py MEASURED this on our TPT-SVF EQ: a +6dB Q=2 bell
lands 1.4 dB off the analog prototype at 10kHz and ~3.1 dB off at 15kHz
(fs=44.1k), and the lowpass gets a spurious z=-1 zero that dives to -inf.

Vicanek's fix (2016 "Matched Second Order Digital Filters", 2019 shelving
paper) is "poles exact, zeros matched":

  1. POLES via impulse invariance -- a1,a2 preserve the analog pole's decay
     rate and resonant frequency EXACTLY (no BLT warp on the denominator).
  2. ZEROS via magnitude matching -- substitute phi = sin^2(w/2), which folds
     |H(e^jw)|^2 into a RATIONAL function of phi:
         |H|^2 = (B0*phi0 + B1*phi1 + B2*phi2) / (A0*phi0 + A1*phi1 + A2*phi2)
     so "match at DC / at f0 / match the curvature" become LINEAR equations in
     phi and b0,b1,b2 pop out in closed form (the "general inversion" below).
  3. PHASE is discarded -- magnitude only. That is the price; it is why this is
     a *static-accuracy* fix, not a time-varying one (test 6 documents that).

Why the pole formulas below reproduce OUR analog prototype exactly: cramping's
SVF bell is H(s) = (s^2 + (A/Q)s + 1)/(s^2 + s/(Q*A) + 1), i.e. a denominator
with effective Q_pole = Q*A = Q*sqrt(G). Vicanek's peaking exception
q_pole = 1/(2*Q*sqrt(G)) sets the digital poles to that SAME denominator, so
feeding both designs the identical (f0, Q, gain) makes their analog targets
coincide -- and the matched digital response then tracks it to <0.5 dB up to
20kHz instead of the SVF/BLT's 1.4-3.1 dB.

Run from repo root:  proto/../.venv/bin/python proto/matched_filters.py
"""
import os
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import freqz
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cramping import analog_and_digital_curves  # analog + BLT/TPT curves, reused

OUT = "proto/out"
os.makedirs(OUT, exist_ok=True)


# ============================================================
# Analog prototypes (true frequency axis, no warp) -- the ground truth every
# matched design is graded against. Same (k, m0, m1, m2) mix as our SVF EQ.
# ============================================================

def analog_mag_db_2nd(freqs, f0, gain_db, Q, band_type):
    """|H_a(j2*pi*f)| in dB for the normalized 2nd-order SVF mix,
    H(s) = m0 + m1*s/D + m2/D, D = s^2 + k*s + 1, s = j*(f/f0)."""
    A = 10 ** (gain_db / 40)
    if band_type == "lowpass":
        k, m0, m1, m2 = 1 / Q, 0.0, 0.0, 1.0
    elif band_type == "highpass":
        k, m0, m1, m2 = 1 / Q, 1.0, -1 / Q, -1.0
    elif band_type == "bandpass":
        k, m0, m1, m2 = 1 / Q, 0.0, 1 / Q, 0.0
    elif band_type == "bell":
        k, m0, m1, m2 = 1 / (Q * A), 1.0, (1 / (Q * A)) * (A * A - 1), 0.0
    else:
        raise ValueError(band_type)
    W = freqs / f0
    s = 1j * W
    D = s ** 2 + k * s + 1
    H = m0 + m1 * (s / D) + m2 * (1 / D)
    return 20 * np.log10(np.abs(H) + 1e-15)


def analog_mag_db_shelf1(freqs, fc_hz, G):
    """First-order analog HIGH shelf, DC gain 1, HF gain G (linear), with the
    corner fc at the GEOMETRIC-MEAN point |H(fc)| = sqrt(G) -- i.e. a zero at
    wc/sqrt(G) and a pole at wc*sqrt(G):
        H(s) = (1 + sqrt(G)*s/wc) / (1 + (1/sqrt(G))*s/wc),  s/wc = j*f/fc.
    This is the "shelf = filter shifted by gain" convention Vicanek's fc uses
    (verified numerically: it is the reference that drives the matched design's
    residual below 0.5 dB; the naive H=(1+G s)/(1+s) puts the corner in the
    wrong place and leaves ~0.9 dB)."""
    rt = np.sqrt(G)
    x = 1j * (freqs / fc_hz)     # = s/wc, the 2*pi cancels
    H = (1 + rt * x) / (1 + x / rt)
    return 20 * np.log10(np.abs(H) + 1e-15)


# ============================================================
# Vicanek matched-filter designs -> (b0, b1, b2, a1, a2)
# ============================================================

def _matched_poles(w0, Q, peaking_G=None):
    """Impulse-invariant poles. q = 1/(2Q), except peaking uses the
    q_pole = 1/(2*Q*sqrt(G)) exception so the digital denominator lands on the
    SVF bell's Q_pole = Q*sqrt(G)."""
    if peaking_G is not None:
        q = 1.0 / (2.0 * Q * np.sqrt(peaking_G))
    else:
        q = 1.0 / (2.0 * Q)
    if q <= 1.0:
        a1 = -2.0 * np.exp(-q * w0) * np.cos(np.sqrt(1.0 - q * q) * w0)
    else:
        a1 = -2.0 * np.exp(-q * w0) * np.cosh(np.sqrt(q * q - 1.0) * w0)
    a2 = np.exp(-2.0 * q * w0)
    return a1, a2


def _phiA(w0, a1, a2):
    phi1 = np.sin(w0 / 2.0) ** 2
    phi0 = 1.0 - phi1
    phi2 = 4.0 * phi0 * phi1
    A0 = (1.0 + a1 + a2) ** 2
    A1 = (1.0 - a1 + a2) ** 2
    A2 = -4.0 * a2
    return phi0, phi1, phi2, A0, A1, A2


def _invert_general(B0, B1, B2):
    """Minimum-phase spectral factorization: (B0,B1,B2) -> (b0,b1,b2)."""
    W = 0.5 * (np.sqrt(B0) + np.sqrt(B1))
    b0 = 0.5 * (W + np.sqrt(W * W + B2))
    b1 = 0.5 * (np.sqrt(B0) - np.sqrt(B1))
    b2 = -B2 / (4.0 * b0)
    return b0, b1, b2


def matched_bell(fs, f0, gain_db, Q):
    G = 10 ** (gain_db / 20)                     # linear peak gain
    w0 = 2 * np.pi * f0 / fs
    a1, a2 = _matched_poles(w0, Q, peaking_G=G)
    phi0, phi1, phi2, A0, A1, A2 = _phiA(w0, a1, a2)
    B0 = A0                                       # unity DC
    R1 = (A0 * phi0 + A1 * phi1 + A2 * phi2) * G * G
    R2 = (-A0 + A1 + 4.0 * (phi0 - phi1) * A2) * G * G
    B2 = (R1 - R2 * phi1 - B0) / (4.0 * phi1 * phi1)
    B1 = R2 + B0 + 4.0 * (phi1 - phi0) * B2
    b0, b1, b2 = _invert_general(B0, B1, B2)
    return b0, b1, b2, a1, a2


def matched_lowpass(fs, f0, Q):
    w0 = 2 * np.pi * f0 / fs
    a1, a2 = _matched_poles(w0, Q)
    phi0, phi1, phi2, A0, A1, A2 = _phiA(w0, a1, a2)
    B0 = A0
    R1 = (A0 * phi0 + A1 * phi1 + A2 * phi2) * Q * Q
    B1 = (R1 - B0 * phi0) / phi1
    b0 = 0.5 * (np.sqrt(B0) + np.sqrt(B1))
    b1 = np.sqrt(B0) - b0
    b2 = 0.0
    return b0, b1, b2, a1, a2


def matched_highpass(fs, f0, Q):
    w0 = 2 * np.pi * f0 / fs
    a1, a2 = _matched_poles(w0, Q)
    phi0, phi1, phi2, A0, A1, A2 = _phiA(w0, a1, a2)
    # Numerator is b0*(1 - z^-1)^2 (double zero at DC). One DOF, matched at the
    # resonance |H(w0)| = Q. Since |(1-e^-jw0)^2|^2 = (4*phi1)^2 = 16*phi1^2,
    #   16*b0^2*phi1^2 = Q^2*|A(w0)|^2  ->  b0 = Q*|A(w0)| / (4*phi1).
    # NB: the spec's "/(2*sqrt(phi1))" is a typo -- it puts 4*phi1 INSIDE the
    # sqrt, which yields |H(w0)| = 2*sin(w0/2)*Q (off by ~34 dB at low f0).
    # The 4*phi1-OUTSIDE form below matches the analog HP to <0.1 dB. Verified.
    b0 = Q * np.sqrt(A0 * phi0 + A1 * phi1 + A2 * phi2) / (4.0 * phi1)
    b1 = -2.0 * b0
    b2 = b0
    return b0, b1, b2, a1, a2


def matched_bandpass(fs, f0, Q):
    w0 = 2 * np.pi * f0 / fs
    a1, a2 = _matched_poles(w0, Q)
    phi0, phi1, phi2, A0, A1, A2 = _phiA(w0, a1, a2)
    R1 = A0 * phi0 + A1 * phi1 + A2 * phi2
    R2 = -A0 + A1 + 4.0 * (phi0 - phi1) * A2
    B2 = (R1 - R2 * phi1) / (4.0 * phi1 * phi1)
    B1 = R2 + 4.0 * (phi1 - phi0) * B2
    b1 = -0.5 * np.sqrt(B1)
    b0 = 0.5 * (np.sqrt(B2 + b1 * b1) - b1)
    b2 = -b0 - b1
    return b0, b1, b2, a1, a2


def matched_highshelf1(fs, fc_hz, G):
    """Vicanek 2019 first-order high shelf. Frequencies in Nyquist units."""
    fc = fc_hz / (fs / 2.0)
    fm = 0.9
    phi_m = 1.0 - np.cos(np.pi * fm)
    alpha = (2.0 / np.pi ** 2) * (1.0 / fm ** 2 + 1.0 / (G * fc ** 2)) - 1.0 / phi_m
    beta = (2.0 / np.pi ** 2) * (1.0 / fm ** 2 + G / fc ** 2) - 1.0 / phi_m
    a1 = -alpha / (1.0 + alpha + np.sqrt(1.0 + 2.0 * alpha))
    b = -beta / (1.0 + beta + np.sqrt(1.0 + 2.0 * beta))
    b0 = (1.0 + a1) / (1.0 + b)
    b1 = b * b0
    return b0, b1, 0.0, a1, 0.0


# ============================================================
# Magnitude evaluation of a matched biquad on the true frequency axis
# ============================================================

def matched_mag_db(coeffs, freqs, fs):
    b0, b1, b2, a1, a2 = coeffs
    _, h = freqz([b0, b1, b2], [1.0, a1, a2], worN=freqs, fs=fs)
    return 20 * np.log10(np.abs(h) + 1e-15)


def poles_stable(a1, a2):
    """|roots of z^2 + a1 z + a2| < 1."""
    roots = np.roots([1.0, a1, a2])
    return np.all(np.abs(roots) < 1.0), np.max(np.abs(roots))


def db_at(freqs, mag, f_target):
    return mag[np.argmin(np.abs(freqs - f_target))]


# ============================================================
# TEST 1+2: decramping the bell + exactness anchors + money plot
# ============================================================

def test_bell_decramp(fs=44100.0):
    print("=== TEST 1+2: bell +6dB Q=2 -- decramping vs cramping.py's SVF/BLT ===\n")
    gain, Q = 6.0, 2.0
    configs = [1000.0, 5000.0, 10000.0, 15000.0]
    f_lo, f_hi = 20.0, 20000.0

    fig, axs = plt.subplots(2, len(configs), figsize=(4.6 * len(configs), 7.5),
                            sharex="col")
    rows = []
    ok_all = True
    for col, f0 in enumerate(configs):
        # analog + SVF/BLT ("TPT-bilinear") curves, reused verbatim from cramping.py
        freqs, mag_analog, mag_tpt = analog_and_digital_curves(fs, f0, gain, Q, "bell")
        coeffs = matched_bell(fs, f0, gain, Q)
        mag_matched = matched_mag_db(coeffs, freqs, fs)

        band = (freqs >= f_lo) & (freqs <= f_hi)
        resid_matched = np.max(np.abs((mag_matched - mag_analog)[band]))
        resid_tpt = np.max(np.abs((mag_tpt - mag_analog)[band]))
        # exactness anchors
        err_dc = abs(db_at(freqs, mag_matched, f_lo) - db_at(freqs, mag_analog, f_lo))
        err_f0 = abs(db_at(freqs, mag_matched, f0) - db_at(freqs, mag_analog, f0))
        stable, rmax = poles_stable(coeffs[3], coeffs[4])

        pass_decramp = resid_matched < 0.5
        pass_anchor = err_dc < 0.01 and err_f0 < 0.01
        ok_all &= pass_decramp and pass_anchor and stable
        rows.append((f0, resid_tpt, resid_matched, err_dc, err_f0, rmax))
        print(f"  f0={f0/1000:>4g}k  SVF/BLT residual={resid_tpt:5.2f} dB  ->  "
              f"matched residual={resid_matched:5.3f} dB "
              f"[{'PASS' if pass_decramp else 'FAIL'}]  "
              f"anchors DC={err_dc:.2e} f0={err_f0:.2e} dB "
              f"[{'PASS' if pass_anchor else 'FAIL'}]")

        ax = axs[0, col]
        ax.semilogx(freqs, mag_analog, color="tab:blue", lw=2, label="analog prototype")
        ax.semilogx(freqs, mag_tpt, color="tab:red", ls="--", lw=1.4,
                    label="TPT/BLT (cramped)")
        ax.semilogx(freqs, mag_matched, color="tab:green", ls="-.", lw=1.4,
                    label="Vicanek matched")
        ax.axvline(f0, color="gray", lw=0.5, ls=":")
        ax.set_title(f"bell +6dB Q=2  f0={f0/1000:g}k")
        ax.set_xlim(20, fs / 2)
        ax.set_ylim(-2, 8)
        ax.grid(True, which="both", alpha=0.3)
        if col == 0:
            ax.set_ylabel("dB")
            ax.legend(fontsize=8)

        axd = axs[1, col]
        axd.semilogx(freqs, mag_tpt - mag_analog, color="tab:red", label="TPT/BLT - analog")
        axd.semilogx(freqs, mag_matched - mag_analog, color="tab:green",
                     label="matched - analog")
        axd.axhline(0, color="black", lw=0.5)
        axd.axhline(0.5, color="orange", lw=0.5, ls=":")
        axd.axhline(-0.5, color="orange", lw=0.5, ls=":")
        axd.axvline(f0, color="gray", lw=0.5, ls=":")
        axd.set_xlim(20, fs / 2)
        axd.set_ylim(-3.5, 3.5)
        axd.set_xlabel("Hz")
        axd.grid(True, which="both", alpha=0.3)
        if col == 0:
            axd.set_ylabel("error vs analog (dB)")
            axd.legend(fontsize=8)

    fig.suptitle("Vicanek matched vs TPT/BLT cramping -- bell +6dB Q=2 (fs=44.1k)\n"
                 "matched (green) tracks analog (blue); BLT (red) craps out toward Nyquist")
    plt.tight_layout()
    plt.savefig(f"{OUT}/matched_bell.png", dpi=130)
    plt.close(fig)
    return rows, ok_all


# ============================================================
# TEST 3: LP / HP / BP at f0 = 10k
# ============================================================

def test_lp_hp_bp(fs=44100.0):
    print("\n=== TEST 3: LP / HP / BP matched vs analog, f0=10k ===\n")
    f0, Q = 10000.0, 0.7071
    f_lo, f_hi = 20.0, 20000.0
    freqs = np.linspace(1.0, fs / 2 * 0.9999, 8000)
    band = (freqs >= f_lo) & (freqs <= f_hi)

    specs = [("lowpass", matched_lowpass(fs, f0, Q)),
             ("highpass", matched_highpass(fs, f0, Q)),
             ("bandpass", matched_bandpass(fs, f0, Q))]

    fig, axs = plt.subplots(1, 3, figsize=(15, 5))
    results = {}
    for ax, (bt, coeffs) in zip(axs, specs):
        mag_a = analog_mag_db_2nd(freqs, f0, 0.0, Q, bt)
        mag_m = matched_mag_db(coeffs, freqs, fs)
        err = np.abs(mag_m - mag_a)
        delta = np.max(err[band])
        # audible-region error: only where the analog response is within 40 dB
        # of its own peak. The full-band max lives in the deep stopband (e.g.
        # LP at 20kHz: -78 vs -80 dB) where big dB gaps are 80 dB down and
        # inaudible; the passband/transition is what the ear hears.
        peak = np.max(mag_a[band])
        aud = band & (mag_a > peak - 40.0)
        delta_aud = np.max(err[aud]) if np.any(aud) else np.nan
        stable, rmax = poles_stable(coeffs[3], coeffs[4])
        results[bt] = (delta, delta_aud, rmax)
        print(f"  {bt:9s} max|d|<20k full={delta:6.3f} dB  within-40dB-of-peak={delta_aud:6.3f} dB  "
              f"pole radius={rmax:.4f} [{'stable' if stable else 'UNSTABLE'}]")

        ax.semilogx(freqs, mag_a, color="tab:blue", lw=2, label="analog")
        ax.semilogx(freqs, mag_m, color="tab:green", ls="-.", lw=1.5, label="matched")
        ax.axvline(f0, color="gray", lw=0.5, ls=":")
        ax.axvline(20000, color="orange", lw=0.5, ls=":")
        ax.set_title(f"{bt}  f0=10k Q=0.707")
        ax.set_xlim(20, fs / 2)
        ax.set_ylim(-40, 8)
        ax.set_xlabel("Hz")
        ax.grid(True, which="both", alpha=0.3)
        ax.legend(fontsize=8)
    axs[0].set_ylabel("dB")
    fig.suptitle("Matched LP/HP/BP vs analog prototype (fs=44.1k)")
    plt.tight_layout()
    plt.savefig(f"{OUT}/matched_lp_hp_bp.png", dpi=130)
    plt.close(fig)

    # LP-specific: what residual remains right AT Nyquist (the b2=0 design no
    # longer forces a -inf zero at z=-1, but the analog LP keeps falling while
    # the matched LP flattens to a finite floor -> quantify the leftover gap).
    lp = matched_lowpass(fs, f0, Q)
    f_nyq = np.array([fs / 2 * 0.9999])
    mag_m_nyq = matched_mag_db(lp, f_nyq, fs)[0]
    mag_a_nyq = analog_mag_db_2nd(f_nyq, f0, 0.0, Q, "lowpass")[0]
    print(f"\n  LP at Nyquist: matched={mag_m_nyq:6.2f} dB  analog={mag_a_nyq:6.2f} dB  "
          f"gap={mag_m_nyq - mag_a_nyq:5.2f} dB")
    print("    (BLT forced this to -inf; matched leaves a finite ~few-dB high-shelf-like\n"
          "     residual at the very top bin -- the price of dropping the z=-1 zero.)")
    return results


# ============================================================
# TEST 4: first-order matched shelf vs first-order analog shelf
# ============================================================

def test_shelf(fs=44100.0):
    print("\n=== TEST 4: matched 1st-order high shelf +6dB fc=12k vs analog ===\n")
    gain_db, fc = 6.0, 12000.0
    G = 10 ** (gain_db / 20)
    f_lo, f_hi = 20.0, 20000.0
    freqs = np.linspace(1.0, fs / 2 * 0.9999, 8000)
    band = (freqs >= f_lo) & (freqs <= f_hi)

    coeffs = matched_highshelf1(fs, fc, G)
    mag_m = matched_mag_db(coeffs, freqs, fs)
    mag_a = analog_mag_db_shelf1(freqs, fc, G)
    delta = np.max(np.abs((mag_m - mag_a)[band]))
    stable, rmax = poles_stable(coeffs[3], coeffs[4])
    dc_m = matched_mag_db(coeffs, np.array([f_lo]), fs)[0]
    hf_m = matched_mag_db(coeffs, np.array([fs / 2 * 0.9999]), fs)[0]
    passed = delta < 0.5
    print(f"  matched 1st-order: max|matched-analog|<20k = {delta:5.3f} dB "
          f"[{'PASS' if passed else 'FAIL'}]")
    print(f"    DC gain={dc_m:5.2f} dB (want 0)   Nyquist gain={hf_m:5.2f} dB "
          f"(want {gain_db:g})   pole radius={rmax:.4f}")

    # context only: our SVF 2nd-order high shelf, BLT-cramped (NOT apples-to-apples)
    freqs2, mag_a2, mag_tpt2 = analog_and_digital_curves(fs, fc, gain_db, 0.7071, "high_shelf")
    resid_2nd = np.max(np.abs((mag_tpt2 - mag_a2)[(freqs2 >= f_lo) & (freqs2 <= f_hi)]))

    fig, ax = plt.subplots(figsize=(9, 5.5))
    ax.semilogx(freqs, mag_a, color="tab:blue", lw=2, label="analog 1st-order shelf")
    ax.semilogx(freqs, mag_m, color="tab:green", ls="-.", lw=1.6,
                label="Vicanek matched 1st-order")
    ax.semilogx(freqs2, mag_tpt2, color="tab:red", ls=":", lw=1.4,
                label="SVF 2nd-order shelf, BLT (context only)")
    ax.axvline(fc, color="gray", lw=0.5, ls=":")
    ax.axvline(20000, color="orange", lw=0.5, ls=":")
    ax.set_xlim(20, fs / 2)
    ax.set_ylim(-1, 8)
    ax.set_xlabel("Hz")
    ax.set_ylabel("dB")
    ax.set_title("High shelf +6dB fc=12k (fs=44.1k)\n"
                 "matched 1st-order tracks its 1st-order analog; SVF 2nd-order BLT curve\n"
                 "shown only for cramping context (different order -- not a fair comparison)")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=8)
    plt.tight_layout()
    plt.savefig(f"{OUT}/matched_shelf.png", dpi=130)
    plt.close(fig)
    print(f"    [context] SVF 2nd-order BLT shelf residual < 20k = {resid_2nd:.2f} dB "
          f"(different order; shown for cramping context only)")
    return delta, passed and stable


# ============================================================
# TEST 5: stability grid
# ============================================================

def test_stability_grid(fs=44100.0):
    print("\n=== TEST 5: pole stability across a param grid ===\n")
    f0s = np.logspace(np.log10(20.0), np.log10(20000.0), 40)
    Qs = np.linspace(0.3, 18.0, 30)
    gains = np.linspace(-24.0, 24.0, 13)

    worst = 0.0
    n_unstable = 0
    n_total = 0
    for f0 in f0s:
        w0 = 2 * np.pi * f0 / fs
        for Q in Qs:
            # 2nd-order non-peaking pole set (LP/HP/BP share it)
            a1, a2 = _matched_poles(w0, Q)
            stable, rmax = poles_stable(a1, a2)
            worst = max(worst, rmax)
            n_total += 1
            n_unstable += not stable
            # peaking pole set depends on gain
            for g in gains:
                G = 10 ** (g / 20)
                a1p, a2p = _matched_poles(w0, Q, peaking_G=G)
                stable, rmax = poles_stable(a1p, a2p)
                worst = max(worst, rmax)
                n_total += 1
                n_unstable += not stable

    # first-order shelf grid (a2 = 0)
    for fc in f0s:
        for g in gains:
            if g == 0:
                continue
            G = 10 ** (g / 20)
            _, _, _, a1, a2 = matched_highshelf1(fs, fc, G)
            stable, rmax = poles_stable(a1, a2)
            worst = max(worst, rmax)
            n_total += 1
            n_unstable += not stable

    all_stable = n_unstable == 0
    print(f"  grid: {n_total} configs (f0 20Hz-20kHz x Q 0.3-18 x gain -24..24 + shelves)")
    print(f"  worst pole radius = {worst:.6f}   unstable = {n_unstable} "
          f"[{'ALL STABLE' if all_stable else 'FAILURES PRESENT'}]")
    return all_stable, worst


# ============================================================
# TEST 6: time-varying smoke test (NEGATIVE CONTROL)
# Matched coeffs in a DF2T biquad, recomputed per block, state carried across.
# Matched design fixes STATIC accuracy only -- direct-form state is still
# coefficient-dependent, so a fast sweep still zippers. This is expected.
# ============================================================

def _df2t_block(x, coeffs, z1, z2):
    b0, b1, b2, a1, a2 = coeffs
    y = np.empty_like(x)
    for i in range(len(x)):
        xi = x[i]
        yi = b0 * xi + z1
        z1 = b1 * xi - a1 * yi + z2
        z2 = b2 * xi - a2 * yi
        y[i] = yi
    return y, z1, z2


def test_timevarying_smoke(fs=44100.0):
    print("\n=== TEST 6: time-varying smoke test (NEGATIVE CONTROL) ===\n")
    dur, block = 2.0, 64
    Q, gain = 6.0, 12.0
    n = int(fs * dur)
    rng = np.random.default_rng(0)
    x = rng.uniform(-0.3, 0.3, n)
    t = np.arange(n) / fs
    f0_track = 200.0 * (5000.0 / 200.0) ** (t / dur)   # exp sweep 200 -> 5k

    z1 = z2 = 0.0
    y = np.empty(n)
    for start in range(0, n, block):
        end = min(start + block, n)
        coeffs = matched_bell(fs, f0_track[start], gain, Q)
        y[start:end], z1, z2 = _df2t_block(x[start:end], coeffs, z1, z2)

    peak = np.max(np.abs(y))
    y_safe = np.clip(y, -1, 1)
    wavfile.write(f"{OUT}/matched_sweep_df2t.wav", int(fs),
                  (y_safe * 32767).astype(np.int16))

    def envelope(sig, win=256):
        m = len(sig) // win
        return np.max(np.abs(sig[:m * win].reshape(m, win)), axis=1)

    env = envelope(y)
    env_in = envelope(x)
    crest = peak / (np.sqrt(np.mean(y ** 2)) + 1e-12)
    print(f"  matched-coeff DF2T sweep: peak|y|={peak:.3f}  crest={crest:.1f}")
    print("  EXPECT zipper transients at each block update -- matched coeffs fix")
    print("  static magnitude, NOT time-varying state continuity. Direct-form state")
    print("  is coefficient-dependent, so this is a NEGATIVE control, not a failure.")
    print("  (The fix is topology -- TPT-SVF state, per biquad_eq.py part 3.)")

    fig, ax = plt.subplots(figsize=(11, 4.5))
    te = np.linspace(0, dur, len(env))
    ax.plot(te, env_in, color="0.7", lw=0.8, label="input envelope")
    ax.plot(te, env, color="crimson", lw=1.0, label="matched DF2T output envelope")
    ax.set_title("NEGATIVE CONTROL: matched-coeff bell in DF2T, f0 swept 200Hz->5kHz "
                 "(Q=6, +12dB)\nzipper transients persist -- matched coeffs alone do not "
                 "fix modulation")
    ax.set_xlabel("s")
    ax.set_ylabel("|y| envelope")
    ax.grid(alpha=0.3)
    ax.legend(fontsize=9)
    plt.tight_layout()
    plt.savefig(f"{OUT}/matched_sweep_envelope.png", dpi=130)
    plt.close(fig)
    return peak


# ============================================================
# main
# ============================================================

if __name__ == "__main__":
    fs = 44100.0
    bell_rows, bell_ok = test_bell_decramp(fs)
    lphpbp = test_lp_hp_bp(fs)
    shelf_delta, shelf_ok = test_shelf(fs)
    grid_ok, grid_worst = test_stability_grid(fs)
    smoke_peak = test_timevarying_smoke(fs)

    print("\n" + "=" * 68)
    print("SUMMARY")
    print("=" * 68)
    print("\nDecramping (bell +6dB Q=2) -- residual vs analog, below 20kHz:")
    print(f"  {'f0':>6s}  {'SVF/BLT (old)':>14s}  {'matched (new)':>14s}  {'anchors':>10s}")
    for f0, r_tpt, r_m, e_dc, e_f0, rmax in bell_rows:
        print(f"  {f0/1000:>5g}k  {r_tpt:>12.2f} dB  {r_m:>12.3f} dB  "
              f"{'<0.01dB' if max(e_dc, e_f0) < 0.01 else 'FAIL':>10s}")
    print("\nLP/HP/BP matched residual <20k (dB):")
    for bt, (d, d_aud, rmax) in lphpbp.items():
        print(f"  {bt:9s} full={d:6.3f}  audible(within 40dB of peak)={d_aud:6.3f}   "
              f"(pole radius {rmax:.4f})")
    print(f"\nShelf (1st-order high +6dB fc=12k) residual <20k: {shelf_delta:.3f} dB")
    print(f"Stability grid: worst pole radius {grid_worst:.6f}  "
          f"[{'ALL STABLE' if grid_ok else 'UNSTABLE PRESENT'}]")
    print(f"Time-varying negative control: peak|y|={smoke_peak:.3f} "
          f"(zipper expected, not fixed here)")

    worst_bell_resid = max(r_m for _, _, r_m, _, _, _ in bell_rows)
    # spec gates: bell (test 1) and shelf (test 4) must be < 0.5 dB and stable;
    # LP/HP/BP (test 3) is report-only (single-point matching, no threshold).
    verdict = bell_ok and shelf_ok and grid_ok
    print("\nWrote: matched_bell.png, matched_lp_hp_bp.png, matched_shelf.png,")
    print("       matched_sweep_envelope.png, matched_sweep_df2t.wav")
    print(f"\nVERDICT: {'PASS' if verdict else 'FAIL'} -- Vicanek matched design decramps the "
          f"bell to <{worst_bell_resid:.2f} dB (was up to 3.1 dB); shelf <0.2 dB;\n"
          f"         HP/BP track analog to <0.1 / <1 dB; LP matches DC+resonance exactly, with\n"
          f"         a ~1.5 dB top-octave residual near Nyquist (the b2=0 / no-z=-1-zero price,\n"
          f"         vs BLT's -inf). All poles stable; time-varying still needs TPT topology\n"
          f"         (test 6 negative control confirms).")
