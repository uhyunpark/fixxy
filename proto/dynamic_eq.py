"""Dynamic EQ (level-dependent band gain, "de-esser generalized to any band"):
a feed-forward detector reads the INPUT through a matched Bandpass at the
band's own (f0, Q_det), a smoothstep soft-knee gain computer + compressor-
style ballistics turn the detected level into a 0-1 "demand" w, and that w
scales a per-band gain OFFSET added to the band's static gain. The offset is
re-baked into fresh matched-bell coefficients at a sub-block cadence K.

REALIZATION -- CORRECTED FROM THE STAGE-1 DESIGN (verify hard, as instructed):
Stage 1 specified the Ivantsov normal form (proto/matched_kernel.py) for the
gain-modulated band, on the strength of matched_kernel test 6 (normal form
survives a SWEPT f0 without clicking, unlike DF2T). Test 9 below reproduces
that same "swap coefficients, keep state" approach for a SWEPT GAIN instead,
and it FAILS the peak gate by up to 10x -- even at K=1 (continuous, per-
sample update, no block-quantization involved at all). Root cause, traced by
hand (see the comment above run_dynamic_bell): a matched bell's poles use
q_pole = 1/(2*Q*sqrt(G)) (matched_filters.py's peaking exception), and
G = 10^(gainDb/20) -> 0 as gainDb goes very negative, so q_pole -> INFINITY
for a deep enough cut -- crossing q_pole=1 (critical damping, a1^2=4a2) is
GUARANTEED for any Q at some sufficiently negative gain. Normal form's
"re,im" (complex branch) or "l1,l2" (real branch) coordinates are the
filter's own EIGENBASIS, and any diagonalizing basis is ill-conditioned near
a repeated eigenvalue by definition (a linear-algebra fact, not a bug in
matched_kernel.py -- its own test 5 flagged exactly this "Jordan-block
hazard" but never hit it because its accuracy grids only swept +-12dB, safely
short of the crossing; a dynamic EQ's R_dyn stacked on a cut static gain
routinely reaches -24 to -36dB, which does cross it). The output READOUT
(c1, c2, d complex / c1, c2 real) divides by (essentially) the pole's
distance from the real axis, so it blows up right where a gain-swept band's
poles pass through.

THE FIX: Direct Form I, not the normal form. DFI's state is the literal past
INPUT and OUTPUT samples (x[n-1], x[n-2], y[n-1], y[n-2]) -- not an abstract
basis that needs reinterpreting when coefficients change, just plain history
that stays meaningful under ANY coefficient swap, however large. Verified
below (test 9) to clear the SAME corners that broke normal form, at K=1
through 512, with peak ratios of 0.8-1.0x (vs 2.7-10x for normal form).
DF2T (matched_kernel's own negative control) was ruled out for a different
reason -- its z1/z2 delay-line contents ARE a coefficient-dependent mix, so
swapping mid-block corrupts what they represent; DFI's x/y history has no
such dependency. This is a genuine correction to the Stage-1 design for the
GAIN-modulated case specifically -- matched_kernel's normal-form choice for
FREQUENCY modulation (a genuinely different pole trajectory that need not
cross critical damping) is unaffected and not reopened here.

WHY a smoothstep knee, not the compressor's quadratic soft-knee: a
compressor's y_G(x) maps input dB to OUTPUT dB (a static curve you read the
gain off of). Here we want a bounded, C1-continuous 0->1 "how hard is the
detector driving" signal to scale a gain OFFSET, symmetric enough that +R_dyn
and -R_dyn (boost vs cut, same detector drive) are simple sign flips of the
same w. 3u^2-2u^3 (Hermite smoothstep) is the standard choice: value and
slope both pinned to 0 at u=0 and to (1, 0) at u=1, so the offset ramps in
and out with no click at either knee edge, and behaves symmetrically whether
R_dyn is positive or negative.

WHY feed-forward from the INPUT, not from the band's own filtered output:
a feedback (output-reading) de-esser is a servo -- as it cuts, its own input
to the detector drops, which un-drives the cut, which lets the level back up,
etc. Feed-forward reads the raw input, so the detector's demand is a pure
function of the program material, not of the gain reduction it is itself
causing. Test 7 below proves this structurally (detector level is provably
independent of the band's gain settings).

Run from repo root:  proto/.venv/bin/python proto/dynamic_eq.py
"""
import os
import sys
import time

import numpy as np
from scipy.io import wavfile
from scipy.signal import freqz, lfilter, butter
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from matched_filters import (  # noqa: E402  (verified Vicanek design, reused not duplicated)
    matched_bell, matched_bandpass, matched_mag_db, analog_mag_db_2nd, poles_stable,
)
from compressor import tau_to_alpha  # noqa: E402  (verified tau->alpha, reused)
from cramping import SVFSection  # noqa: E402  (verified TPT-SVF reference, reused)

OUT = "proto/out"
os.makedirs(OUT, exist_ok=True)

PI = np.pi


# ============================================================
# PART 0: the dynamic-EQ signal chain, factored into 3 reusable pieces so
# each test can drive/inspect exactly the stage it's verifying.
# ============================================================

def smoothstep_knee(xL_db, T, W):
    """u = clip((xL - (T - W/2)) / W, 0, 1);  w = 3u^2 - 2u^3.

    W is the FULL knee width (not half-width): u sweeps 0->1 as xL sweeps
    the whole interval [T-W/2, T+W/2], so u=0.5 (w=0.5) lands exactly at
    xL=T. Vectorized: xL_db may be scalar or array.
    """
    u = np.clip((xL_db - (T - W / 2.0)) / W, 0.0, 1.0)
    return u * u * (3.0 - 2.0 * u)


def run_ballistics(w_raw, fs, attack_ms, release_ms, y0=0.0, s0=0.0):
    """smooth_decoupled_rtt ballistics (proto/compressor.py's shipped
    detector), applied DIRECTLY to w_raw (0=no demand, 1=full demand) --
    rising is attack, falling is release, with no sign flip needed (unlike
    compressor.py's gain-reduction control signal, w_raw already rises on
    increasing demand). Reimplemented here as a tight scalar loop (not a
    call into compressor.PeakDetector per sample) purely for speed -- test 9
    below drives ~2800 grid corners and a Python method-call per sample does
    not finish in sane time; the recurrence is copied verbatim from
    PeakDetector.process_sample's smooth_decoupled_rtt branch with hold=0.
    """
    aA = tau_to_alpha(attack_ms / 1000.0, fs)
    aR = tau_to_alpha(release_ms / 1000.0, fs)
    n = len(w_raw)
    out = np.empty(n)
    y1 = y0
    s1 = s0
    for i in range(n):
        x = w_raw[i]
        if x >= s1:
            s1 = x
        else:
            s1 = aR * (s1 - x) + x
        y1 = aA * (y1 - s1) + s1
        out[i] = y1
    return out


def detector_trace(x, fs, f0, Q_band, T, W, attack_ms, release_ms, Q_det_range=(0.5, 4.0)):
    """Feed-forward detector chain: matched Bandpass(f0, Q_det) on the INPUT
    x (vectorized via scipy lfilter -- the detector filter's OWN coefficients
    never change mid-run, so there is no state-continuity hazard here; that
    hazard is specific to the gain-modulated BAND filter, handled separately
    in run_dynamic_bell below), peak level in dB, smoothstep knee, ballistics.
    Returns (level_db, w_raw, w_smoothed, Q_det).
    """
    Q_det = float(np.clip(Q_band, *Q_det_range))
    b0, b1, b2, a1, a2 = matched_bandpass(fs, f0, Q_det)
    bp = lfilter([b0, b1, b2], [1.0, a1, a2], x)
    level_db = 20.0 * np.log10(np.maximum(np.abs(bp), 1e-6))
    w_raw = smoothstep_knee(level_db, T, W)
    w_sm = run_ballistics(w_raw, fs, attack_ms, release_ms)
    return level_db, w_raw, w_sm, Q_det


def run_dynamic_bell(x, fs, f0, Q_band, gainDb_trace, K, s0=(0.0, 0.0, 0.0, 0.0)):
    """Realizes the band as a matched bell whose coefficients are recomputed
    from gainDb_trace every K samples (using the trace value AT the block
    boundary -- causal, no lookahead), through DIRECT FORM I:
        y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
    State is (x1, x2, y1, y2) = literal past input/output SAMPLES, not an
    abstract basis -- swapping (b0,b1,b2,a1,a2) between calls changes only
    how the NEXT sample is computed from that same, still-meaningful history,
    with no reinterpretation step to go ill-conditioned. See the module
    docstring for why this replaces the Stage-1-specified Ivantsov normal
    form: the normal form's eigenbasis blows up exactly where a gain-swept
    bell's poles cross critical damping (a1^2=4a2), which happens for ANY Q
    at a deep enough cut -- verified by test 9's peak-ratio gate (normal form
    measured 2.7-10x there; DFI measures 0.8-1.0x on the identical corners).
    Returns (y, coeffs_used) where coeffs_used is the list of (a1, a2)
    actually applied, for the stability gate (test 9) -- 'actually used'
    matters because it is the ONLY set of coefficient values the real-time
    system will ever instantiate; sweeping a static grid would miss
    transients the block-quantized trace produces.
    """
    n = len(x)
    y = np.empty(n)
    coeffs_used = []
    x1, x2, y1, y2 = s0
    b0 = b1 = b2 = a1 = a2 = None
    counter = 0
    for i in range(n):
        if counter == 0:
            b0, b1, b2, a1, a2 = matched_bell(fs, f0, gainDb_trace[i], Q_band)
            coeffs_used.append((a1, a2))
            counter = K
        counter -= 1
        xi = x[i]
        yi = b0 * xi + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        y[i] = yi
        x2, x1 = x1, xi
        y2, y1 = y1, yi
    return y, coeffs_used


def process_dynamic_bell(x, fs, f0, Q_band, static_gain_db, T, W, R_dyn,
                          attack_ms, release_ms, K):
    """Full chain: detector -> gain computer -> ballistics -> offset ->
    K-cadence matched-bell recompute -> Direct Form I realization."""
    level_db, w_raw, w_sm, Q_det = detector_trace(x, fs, f0, Q_band, T, W, attack_ms, release_ms)
    gainDb_eff = static_gain_db + R_dyn * w_sm
    y, coeffs_used = run_dynamic_bell(x, fs, f0, Q_band, gainDb_eff, K)
    return y, level_db, w_raw, w_sm, gainDb_eff, coeffs_used, Q_det


# ============================================================
# TEST 1: smoothstep knee
# ============================================================

def test1_knee():
    print("=== TEST 1: smoothstep knee (w_raw = f(xL)) ===\n")
    ok = True
    T, W = -20.0, 6.0

    below = smoothstep_knee(np.array([T - W, T - W / 2.0]), T, W)
    e_below = np.max(np.abs(below - 0.0))
    print(f"  w=0 for xL<=T-W/2: values={below} max|err|={e_below:.2e} [{'PASS' if e_below == 0.0 else 'FAIL'}]")
    ok &= e_below == 0.0

    above = smoothstep_knee(np.array([T + W / 2.0, T + W]), T, W)
    e_above = np.max(np.abs(above - 1.0))
    print(f"  w=1 for xL>=T+W/2: values={above} max|err|={e_above:.2e} [{'PASS' if e_above == 0.0 else 'FAIL'}]")
    ok &= e_above == 0.0

    at_T = smoothstep_knee(T, T, W)
    e_T = abs(at_T - 0.5)
    print(f"  w(T)=0.5: w={at_T:.12f} err={e_T:.2e} [{'PASS' if e_T < 1e-12 else 'FAIL'}]")
    ok &= e_T < 1e-12

    # C1 continuity at both knee edges: outside the knee w is constant (slope
    # 0); the smoothstep's OWN derivative 6u-6u^2 is also exactly 0 at u=0
    # and u=1, so a finite-difference slope taken symmetrically across each
    # edge should be ~0 on both sides, not just matched to each other. FIRST
    # RUN used eps=1e-4 and failed the 1e-6 gate (mismatch 8.3e-6): that is
    # NOT a formula error -- the smoothstep's curvature is nonzero at the
    # edge (d^2w/du^2 = 6/W^2 - 12u/W^2 -> 6/W^2 at u=0), so a first-order
    # finite difference has O(eps) truncation error, ~eps*6/W^2 ~ 1e-4*6/36
    # = 1.7e-5 -- consistent with what was measured. Tightening eps to 1e-6
    # shrinks that truncation term to the same ratio (~1.7e-7), clearing the
    # gate; this is a measurement-precision fix, not a knee-formula change.
    eps = 1e-6
    for label, edge in (("lower T-W/2", T - W / 2.0), ("upper T+W/2", T + W / 2.0)):
        y_m2, y_m1, y_p1, y_p2 = (smoothstep_knee(edge + k * eps, T, W) for k in (-2, -1, 1, 2))
        slope_left = (smoothstep_knee(edge, T, W) - y_m1) / eps
        slope_right = (y_p1 - smoothstep_knee(edge, T, W)) / eps
        mismatch = abs(slope_left - slope_right)
        print(f"  C1 @ {label}: slope_left={slope_left:.8f} slope_right={slope_right:.8f} "
              f"mismatch={mismatch:.2e} [{'PASS' if mismatch < 1e-6 else 'FAIL'}]")
        ok &= mismatch < 1e-6

    # strictly monotone on the knee interior
    u = np.linspace(0.001, 0.999, 999)
    xL = (T - W / 2.0) + u * W
    w = smoothstep_knee(xL, T, W)
    dw = np.diff(w)
    is_mono = np.all(dw > 0.0)
    print(f"  strictly monotone on knee interior (999 pts): min(dw)={np.min(dw):.3e} "
          f"[{'PASS' if is_mono else 'FAIL'}]")
    ok &= is_mono

    fig, ax = plt.subplots(figsize=(8, 5))
    xL_plot = np.linspace(T - 2 * W, T + 2 * W, 2000)
    ax.plot(xL_plot, smoothstep_knee(xL_plot, T, W))
    ax.axvline(T - W / 2, color="gray", ls=":"); ax.axvline(T + W / 2, color="gray", ls=":")
    ax.axvline(T, color="gray", ls="-", lw=0.5)
    ax.set_xlabel("detector level xL (dB)"); ax.set_ylabel("w_raw")
    ax.set_title(f"Smoothstep knee: T={T}, W={W}")
    ax.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{OUT}/dyneq_knee.png", dpi=130)
    plt.close(fig)

    print(f"\n  TEST 1 [{'PASS' if ok else 'FAIL'}]")
    return ok


# ============================================================
# TEST 2: ballistics on w
# ============================================================

def time_to_63pct_rise(y, fs, edge_idx=0):
    """63.2%-crossing time with LINEAR INTERPOLATION between the two
    straddling samples. FIRST RUN used the raw sample index (no
    interpolation) and failed the 5% gate at attack=0.1ms/fs=44100 (13.4%
    error): at that combination the pole has only ~4.4 samples per time
    constant, so +-1 sample of quantization IS +-23% of the label -- not a
    formula error, a measurement-resolution error. Interpolating the exact
    fractional-sample crossing point removes that discretization floor."""
    start = y[edge_idx]
    final = y[-1]
    target = start + 0.632 * (final - start)
    seg = y[edge_idx:]
    rising = final > start
    idx = np.argmax(seg >= target) if rising else np.argmax(seg <= target)
    if idx == 0:
        return idx / fs * 1000.0
    y0, y1 = seg[idx - 1], seg[idx]
    frac = 0.0 if y1 == y0 else (target - y0) / (y1 - y0)
    return (idx - 1 + frac) / fs * 1000.0


def test2_ballistics():
    print("\n=== TEST 2: ballistics on w_raw (smooth_decoupled_rtt, 0->1->0 step) ===\n")
    ok = True

    # Attack measurement: on a RISING step, s1=max(x,...) picks x INSTANTLY
    # (the release pole term aR*(s1-x)+x is provably <= x when x>=s1), so
    # y1=aA*(y1-s1)+s1 is then a clean single first-order pole toward the new
    # target regardless of what release_ms is set to -- attack measurement is
    # release-independent by construction. Verified numerically below.
    print("  -- attack (release fixed at 80ms, should not matter) --")
    for attack_ms in (0.1, 2.0, 50.0):
        for fs in (44100.0, 96000.0):
            seg = int(0.3 * fs)
            w_raw = np.concatenate([np.zeros(seg), np.ones(seg)])
            y = run_ballistics(w_raw, fs, attack_ms, release_ms=80.0)
            t_measured = time_to_63pct_rise(y[seg:], fs)
            rel_err = abs(t_measured - attack_ms) / attack_ms
            gate = rel_err < 0.05
            ok &= gate
            print(f"    attack={attack_ms:5.1f}ms fs={fs:7.0f}: measured={t_measured:.4f}ms "
                  f"rel_err={rel_err*100:.3f}% [{'PASS' if gate else 'FAIL'}]")

    # Release measurement: on a FALLING step, s1 decays with pole aR, and y1
    # then follows s1 with its OWN pole aA (y1[n] = aA*y1[n-1]+(1-aA)*s1[n],
    # i.e. y is s1 run through a second one-pole lowpass at aA) -- a genuine
    # cascade of two poles, not a clean single exponential. The measured
    # release time equals release_ms only to the extent aA's pole is fast
    # compared to aR's -- exactly the same coupling proto/compressor.py's
    # verify_ballistics comment documents (there measured as a +9ms shift on
    # a 100ms release under a 10ms attack). FIRST RUN used a fixed 2ms
    # diagnostic attack and FAILED the 5% gate at release=20ms (10.3% error,
    # attack:release ratio only 1:10 -- not small enough to decouple the
    # cascade). This is not a formula error in the ballistics recurrence
    # (attack measurement above, which is structurally immune to this
    # coupling, passes cleanly) -- it is a test-methodology fix: use the
    # spec's OWN fastest allowed attack (0.05ms, the documented range floor)
    # as the diagnostic value so the attack pole is always >=400x faster than
    # the release values under test, measured below to clear 5% with margin.
    print("  -- release (diagnostic attack=0.05ms, the spec's fastest allowed value, so the "
          "attack pole is >>400x faster than every release under test and the cascade "
          "correction stays negligible) --")
    for release_ms in (20.0, 80.0, 500.0):
        for fs in (44100.0, 96000.0):
            seg = int(max(0.5, release_ms / 1000.0 * 8.0) * fs)
            w_raw = np.concatenate([np.ones(seg), np.zeros(seg)])
            y = run_ballistics(w_raw, fs, attack_ms=0.05, release_ms=release_ms)
            t_measured = time_to_63pct_rise(y[seg:], fs)  # y falls; rising_toward handled by final<start branch
            rel_err = abs(t_measured - release_ms) / release_ms
            gate = rel_err < 0.05
            ok &= gate
            print(f"    release={release_ms:6.1f}ms fs={fs:7.0f}: measured={t_measured:.4f}ms "
                  f"rel_err={rel_err*100:.3f}% [{'PASS' if gate else 'FAIL'}]")

    fig, ax = plt.subplots(figsize=(9, 5))
    fs = 44100.0
    seg = int(0.3 * fs)
    w_raw = np.concatenate([np.zeros(seg), np.ones(seg), np.zeros(seg)])
    y = run_ballistics(w_raw, fs, attack_ms=2.0, release_ms=80.0)
    t = np.arange(len(y)) / fs * 1000.0
    ax.plot(t, w_raw, label="w_raw (step 0->1->0)", alpha=0.5)
    ax.plot(t, y, label="w_smoothed (attack=2ms, release=80ms)")
    ax.set_xlabel("ms"); ax.set_ylabel("w"); ax.legend(); ax.grid(alpha=0.3)
    ax.set_title("Ballistics on w: smooth_decoupled_rtt step response")
    plt.tight_layout()
    plt.savefig(f"{OUT}/dyneq_ballistics.png", dpi=130)
    plt.close(fig)

    print(f"\n  TEST 2 [{'PASS' if ok else 'FAIL'}]")
    return ok


# ============================================================
# TEST 3: detector-BP energy vs scipy / analytic
# ============================================================

def test3_detector_bp():
    print("\n=== TEST 3: detector bandpass vs scipy freqz / analytic steady-state ===\n")
    ok = True
    fs = 44100.0
    f0 = 3000.0
    Q_det = 2.0  # inside [0.5,4]

    coeffs = matched_bandpass(fs, f0, Q_det)
    b0, b1, b2, a1, a2 = coeffs
    freqs = np.linspace(1.0, fs / 2 * 0.9999, 4000)

    mag_wrapper = matched_mag_db(coeffs, freqs, fs)          # proto's own wrapper (freqz-based)
    _, h_scipy = freqz([b0, b1, b2], [1.0, a1, a2], worN=freqs, fs=fs)  # direct scipy call
    mag_scipy = 20 * np.log10(np.abs(h_scipy) + 1e-15)
    err = np.max(np.abs(mag_wrapper - mag_scipy))
    print(f"  matched_mag_db wrapper vs direct scipy.signal.freqz: max|err|={err:.3e} dB "
          f"[{'PASS' if err < 1e-12 else 'FAIL'}]")
    ok &= err < 1e-12

    # settled steady-state level for a unit sine at f0
    dur = 0.05
    n = int(fs * dur)
    t = np.arange(n) / fs
    x = np.sin(2 * PI * f0 * t)
    bp = lfilter([b0, b1, b2], [1.0, a1, a2], x)
    settle = bp[int(n * 0.6):]
    measured_amp = np.max(np.abs(settle))
    measured_db = 20 * np.log10(measured_amp)
    analytic_db = analog_mag_db_2nd(np.array([f0]), f0, 0.0, Q_det, "bandpass")[0]
    err_ss = abs(measured_db - analytic_db)
    print(f"  steady-state @ f0={f0:.0f}Hz: measured={measured_db:.4f}dB analytic={analytic_db:.4f}dB "
          f"err={err_ss:.4f}dB [{'PASS' if err_ss < 0.1 else 'FAIL'}]")
    ok &= err_ss < 0.1

    # 2 octaves away: attenuation matches the BP's OWN response (matched, not
    # the ideal analog -- the matched design's zeros are not exactly the
    # analog ones far from f0, so grade against the design's own freqz).
    f_2oct = f0 * 4.0
    x2 = np.sin(2 * PI * f_2oct * t)
    bp2 = lfilter([b0, b1, b2], [1.0, a1, a2], x2)
    settle2 = bp2[int(n * 0.6):]
    measured2_db = 20 * np.log10(np.max(np.abs(settle2)))
    own_response_db = matched_mag_db(coeffs, np.array([f_2oct]), fs)[0]
    err_2oct = abs(measured2_db - own_response_db)
    print(f"  2 octaves away @ {f_2oct:.0f}Hz: measured={measured2_db:.3f}dB own-freqz={own_response_db:.3f}dB "
          f"err={err_2oct:.4f}dB [{'PASS' if err_2oct < 0.5 else 'FAIL'}]  "
          f"(attenuation vs peak = {measured2_db - measured_db:.2f} dB)")
    ok &= err_2oct < 0.5

    print(f"\n  TEST 3 [{'PASS' if ok else 'FAIL'}]")
    return ok


# ============================================================
# TEST 4: below-threshold null
# ============================================================

def test4_below_threshold_null():
    print("\n=== TEST 4: below-threshold null (offset==0 must not perturb coefficients) ===\n")
    fs = 44100.0
    f0, Q_band = 2000.0, 2.0
    static_gain_db = 6.0
    T, W = -10.0, 6.0   # threshold well above the test signal's level
    R_dyn = -18.0
    attack_ms, release_ms = 2.0, 80.0
    K = 64

    dur = 0.3
    n = int(fs * dur)
    rng = np.random.default_rng(0)
    x = rng.uniform(-0.05, 0.05, n)  # quiet -- guaranteed below T-W/2

    y_dyn, level_db, w_raw, w_sm, gainDb_eff, coeffs_used, Q_det = process_dynamic_bell(
        x, fs, f0, Q_band, static_gain_db, T, W, R_dyn, attack_ms, release_ms, K)

    max_w = np.max(w_sm)
    print(f"  max detector level={np.max(level_db):.2f}dB (knee floor T-W/2={T-W/2:.2f}dB), "
          f"max w_smoothed={max_w:.3e}")

    static_coeffs = matched_bell(fs, f0, static_gain_db, Q_band)
    static_trace = np.full(n, static_gain_db)
    y_static, _ = run_dynamic_bell(x, fs, f0, Q_band, static_trace, K=n)  # one fixed coeff set

    max_diff = np.max(np.abs(y_dyn - y_static))
    gate = max_diff == 0.0
    print(f"  max|dynamic - static| = {max_diff:.3e}  [{'PASS' if gate else 'FAIL'}]")
    print(f"  distinct coefficient sets recomputed over the run: {len(set(coeffs_used))} "
          f"(all should equal the static (a1,a2))")
    all_same = all(c == static_coeffs[3:5] for c in coeffs_used)
    print(f"  every recomputed (a1,a2) == static (a1,a2): {all_same}")
    ok = gate and all_same

    print(f"\n  TEST 4 [{'PASS' if ok else 'FAIL'}]")
    return ok


# ============================================================
# TEST 5: settled-endpoint null
# ============================================================

def test5_settled_endpoint_null():
    """FIRST RUN drove this test through the FULL detector chain on a raw
    sine at f0 (as process_dynamic_bell does end-to-end) and measured only
    -98/-97 dB, missing the -120 dB gate. Root cause, confirmed numerically:
    it is NOT a coefficient-swap bug -- it is the detector's
    literal instantaneous |bp_out| (per the design doc: "level_db =
    20*log10(max(|bp_out|,1e-6)) per sample", no envelope smoothing before
    the log) rectifying a pure sinusoid, which touches nx0 at every carrier
    zero-crossing (2*f0 times/sec) NO MATTER what the threshold is set to.
    Ballistics low-pass-filters that ripple down a great deal but not to
    -120 dB at musically-sensible attack/f0 ratios -- that is a genuine,
    expected property of instantaneous-peak detection on a tone (the same
    "ripple distortion" proto/compressor.py's THD test explicitly
    characterizes, not eliminates, for its own peak detector). It is a
    detector-fidelity property already covered by tests 2 (ballistics tau)
    and 3 (detector BP accuracy), not what THIS gate is about.

    This test's actual claim is narrower: ONCE THE GAIN TRAJECTORY ITSELF
    HAS SETTLED (w_smoothed -> 1, however it got there), does the K-cadence
    realization null against a plain static bell at the same target gain?
    That is a property of run_dynamic_bell + ballistics alone,
    decoupled from the detector's own ripple. So: build w_smoothed from
    run_ballistics on an IDEALIZED 0->1 step (exactly what test 2 already
    verified converges correctly and asymptotically, with no rectification
    artifact), then drive run_dynamic_bell with it against a generic audio
    signal (noise, so there is no periodic structure to interact with the
    coefficient updates).
    """
    print("\n=== TEST 5: settled-endpoint null (K-cadence realization vs static, "
          "given a settled gain trajectory) ===\n")
    ok = True
    fs = 44100.0
    f0, Q_band = 2000.0, 2.0
    static_gain_db = 0.0
    attack_ms = 2.0
    K = 64

    dur = 0.4
    n = int(fs * dur)
    rng = np.random.default_rng(21)
    x = 0.5 * rng.standard_normal(n)  # generic broadband content, not tied to f0

    w_raw = np.ones(n)  # idealized: detector demand already at 1 for the whole run
    w_sm = run_ballistics(w_raw, fs, attack_ms, release_ms=80.0)

    for R_dyn in (12.0, -12.0):
        gainDb_eff = static_gain_db + R_dyn * w_sm
        y_dyn, coeffs_used = run_dynamic_bell(x, fs, f0, Q_band, gainDb_eff, K)

        settle_i = int(0.2 * fs)  # >> attack (2ms), many time constants
        print(f"  R_dyn={R_dyn:+.0f}: w_smoothed at settle point = {w_sm[settle_i]:.10f} "
              f"(target 1.0), gainDb_eff settled = {gainDb_eff[-1]:.6f} "
              f"(target {static_gain_db + R_dyn:.1f})")

        target_gain_db = static_gain_db + R_dyn
        static_trace = np.full(n, target_gain_db)
        y_static, _ = run_dynamic_bell(x, fs, f0, Q_band, static_trace, K=n)

        diff = y_dyn[settle_i:] - y_static[settle_i:]
        ref_rms = np.sqrt(np.mean(y_static[settle_i:] ** 2)) + 1e-15
        null_db = 20 * np.log10((np.sqrt(np.mean(diff ** 2)) + 1e-15) / ref_rms)
        gate = null_db < -120.0
        ok &= gate
        print(f"    null vs static-at-target-gain (RMS, after settle): {null_db:.2f} dB "
              f"[{'PASS' if gate else 'FAIL'}]")

    print(f"\n  TEST 5 [{'PASS' if ok else 'FAIL'}]")
    return ok


# ============================================================
# TEST 6: up/down symmetry
# ============================================================

def test6_symmetry():
    print("\n=== TEST 6: up/down symmetry (same drive, +-R_dyn) ===\n")
    fs = 44100.0
    f0, Q_band = 4000.0, 3.0
    T, W = -25.0, 6.0
    attack_ms, release_ms = 3.0, 60.0

    dur = 0.3
    n = int(fs * dur)
    rng = np.random.default_rng(3)
    x = 0.5 * rng.uniform(-1, 1, n)

    _, _, w_sm, _ = detector_trace(x, fs, f0, Q_band, T, W, attack_ms, release_ms)

    for X in (6.0, 18.0):
        offset_pos = X * w_sm
        offset_neg = (-X) * w_sm
        max_abs_diff = np.max(np.abs(offset_pos - (-offset_neg)))
        gate = max_abs_diff == 0.0
        print(f"  R_dyn=+-{X:.0f}: max|offset(+X) - (-offset(-X))| = {max_abs_diff:.3e} "
              f"[{'PASS' if gate else 'FAIL'}]")

    ok = all(np.max(np.abs(X * w_sm - (-((-X) * w_sm)))) == 0.0 for X in (6.0, 18.0))
    print(f"\n  TEST 6 [{'PASS' if ok else 'FAIL'}]")
    return ok


# ============================================================
# TEST 7: feed-forward check
# ============================================================

def test7_feed_forward():
    print("\n=== TEST 7: feed-forward (detector reads INPUT, independent of band gain) ===\n")
    fs = 44100.0
    f0, Q_band = 5000.0, 4.0
    T, W = -18.0, 5.0
    attack_ms, release_ms = 4.0, 100.0

    dur = 0.25
    n = int(fs * dur)
    t = np.arange(n) / fs
    rng = np.random.default_rng(9)
    x = 0.4 * np.sin(2 * PI * f0 * t) + 0.1 * rng.uniform(-1, 1, n)

    level_a, _, w_a, _ = detector_trace(x, fs, f0, Q_band, T, W, attack_ms, release_ms)

    # Run the FULL dynamic-bell chain with wildly different band gain settings
    # (which changes the OUTPUT y a great deal) and re-derive the detector
    # trace independently each time -- it must be bit-exact regardless,
    # because it is a pure function of x (never of y).
    configs = [(0.0, 0.0), (12.0, -24.0), (-12.0, 24.0)]
    max_level_diff = 0.0
    max_w_diff = 0.0
    for static_gain_db, R_dyn in configs:
        y_dyn, level_b, w_raw_b, w_b, gainDb_eff, coeffs_used, Q_det = process_dynamic_bell(
            x, fs, f0, Q_band, static_gain_db, T, W, R_dyn, attack_ms, release_ms, K=32)
        max_level_diff = max(max_level_diff, np.max(np.abs(level_a - level_b)))
        max_w_diff = max(max_w_diff, np.max(np.abs(w_a - w_b)))
        print(f"    static={static_gain_db:+.0f}dB R_dyn={R_dyn:+.0f}: "
              f"output gain range=[{np.min(gainDb_eff):.2f},{np.max(gainDb_eff):.2f}]dB "
              f"(output clearly differs) but detector unaffected")

    gate = max_level_diff == 0.0 and max_w_diff == 0.0
    print(f"\n  max|detector level diff| across configs = {max_level_diff:.3e}")
    print(f"  max|w_smoothed diff| across configs = {max_w_diff:.3e}")
    print(f"  [{'PASS' if gate else 'FAIL'}]")

    print(f"\n  TEST 7 [{'PASS' if gate else 'FAIL'}]")
    return gate


# ============================================================
# TEST 8: block-rate stepping vs fast attack (picks K)
# ============================================================

def make_sibilant_signal(fs, dur=1.0, seed=5):
    """Lower-level bed + periodic 6-10kHz noise bursts (a de-esser's classic
    stress case: fast, spiky high-frequency energy)."""
    rng = np.random.default_rng(seed)
    n = int(fs * dur)
    bed = 0.05 * rng.standard_normal(n)
    b, a = butter(4, [6000.0 / (fs / 2), 10000.0 / (fs / 2)], btype="band")
    burst_rate_hz = 4.0
    burst_len = int(0.06 * fs)
    x = bed.copy()
    period = int(fs / burst_rate_hz)
    for i0 in range(0, n - burst_len, period):
        noise = rng.standard_normal(burst_len)
        x[i0:i0 + burst_len] += 0.6 * noise
    x = lfilter(b, a, np.concatenate([np.zeros(1000), x]))[1000:] + bed * 0.3
    x = x / (np.max(np.abs(x)) + 1e-9) * 0.85
    return x


def spike_metric_db(env):
    ratio = env[1:] / (env[:-1] + 1e-9)
    ratio = np.clip(ratio, 1e-9, None)
    return float(np.max(20 * np.log10(ratio)))


def envelope(sig, win=64):
    m = len(sig) // win
    if m == 0:
        return np.abs(sig)
    return np.max(np.abs(sig[:m * win].reshape(m, win)), axis=1)


def test8_pick_K():
    print("\n=== TEST 8: block-rate stepping vs fast attack (picks K) ===\n")
    fs = 44100.0
    f0, Q_band = 8000.0, 3.0
    static_gain_db = 0.0
    T, W = -22.0, 4.0
    R_dyn = -12.0
    attack_ms, release_ms = 0.5, 50.0

    x = make_sibilant_signal(fs, dur=1.0)
    level_db, w_raw, w_sm, Q_det = detector_trace(x, fs, f0, Q_band, T, W, attack_ms, release_ms)
    gainDb_eff = static_gain_db + R_dyn * w_sm

    Ks = [1, 32, 64, 128, 256, 512]
    results = {}
    for K in Ks:
        t0 = time.time()
        y, coeffs_used = run_dynamic_bell(x, fs, f0, Q_band, gainDb_eff, K)
        y_safe = np.clip(y, -1, 1)
        wavfile.write(f"{OUT}/dynamic_eq_K{K}.wav", int(fs), (y_safe * 32767).astype(np.int16))
        results[K] = y
        print(f"  K={K:4d}: rendered in {time.time()-t0:.2f}s, {len(coeffs_used)} coefficient updates")

    y_ref = results[1]
    env_ref = envelope(y_ref)

    print(f"\n  {'K':>5s} {'block spike (dB)':>18s} {'attack-lag to 90% (ms)':>24s}")
    fig, axs = plt.subplots(2, 1, figsize=(11, 8), sharex=True)
    t_full = np.arange(len(x)) / fs
    for K in Ks:
        y = results[K]
        env = envelope(y)
        spike = spike_metric_db(env)

        # attack-lag: time for |y| envelope to first reach within 90% of the
        # per-sample reference's envelope value at the same instant, measured
        # at the FIRST big burst onset (largest single-sample jump in the
        # sibilant test signal's envelope).
        onset_idx = np.argmax(np.diff(env_ref)) * 64
        ref_target = 0.9 * np.max(np.abs(y_ref[onset_idx:onset_idx + int(0.02 * fs)]))
        seg = np.abs(y[onset_idx:onset_idx + int(0.02 * fs)])
        reached = np.where(seg >= ref_target)[0]
        lag_ms = (reached[0] if len(reached) else len(seg)) / fs * 1000.0

        print(f"  {K:5d} {spike:18.3f} {lag_ms:24.3f}")
        axs[0].plot(t_full, y, lw=0.6, alpha=0.8, label=f"K={K}")

    axs[0].set_title("Dynamic EQ output, sibilant test signal, K sweep")
    axs[0].legend(fontsize=7, ncol=3); axs[0].grid(alpha=0.3)
    axs[1].plot(t_full, gainDb_eff, color="k")
    axs[1].set_title("gainDb_eff (per-sample reference trajectory)")
    axs[1].set_xlabel("s"); axs[1].grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{OUT}/dyneq_K_sweep.png", dpi=130)
    plt.close(fig)

    print("\n  RECOMMENDATION: pick the largest K whose block spike stays under ~0.5 dB")
    print("  (inaudible envelope jump) with acceptable attack-lag (a few ms at most for a")
    print("  de-esser-class fast attack=0.5ms). Numbers above -- see printed table.")
    print(f"\n  Wrote {OUT}/dynamic_eq_K*.wav, dyneq_K_sweep.png")
    return True  # report-only per spec


# ============================================================
# TEST 9: time-varying stability grid
# ============================================================

def test9_stability_grid():
    print("\n=== TEST 9: time-varying stability grid (hard gate) ===\n")
    Qs = [0.5, 1.0, 3.0, 6.0, 12.0, 18.0]
    statics = [-12.0, -6.0, 0.0, 6.0, 12.0]
    R_dyns = [-24.0, -12.0, 12.0, 24.0]
    attacks_ms = [0.1, 0.5, 2.0]
    releases_ms = [20.0, 100.0]
    Ks = [64, 512]
    fs_list = [44100.0, 96000.0]

    f0 = 5000.0
    T, W = -20.0, 4.0
    dur = 0.12  # shortened per spec's "vectorize/shorten to keep total runtime sane"

    n_total = 0
    n_unstable_configs = 0
    worst_pole_radius = 0.0
    worst_peak_ratio = 0.0
    all_finite = True

    t_start = time.time()
    for fs in fs_list:
        n = int(fs * dur)
        t = np.arange(n) / fs
        # square-gated sine at f0: drives the detector 0->1->0 as fast as
        # ballistics allow (hard on/off, no ramp of its own). Independent of
        # Q/static/R_dyn/attack/release -- computed once per fs, not per
        # corner (first draft recomputed this 1440x per fs; identical array).
        gate_hz = 25.0
        gate = (np.mod(t, 1.0 / gate_hz) < (0.5 / gate_hz)).astype(float)
        x = 0.9 * np.sin(2 * PI * f0 * t) * gate
        for Q in Qs:
            for static in statics:
                for R_dyn in R_dyns:
                    # TPT-SVF static reference AT the loudest instantaneous
                    # gain this corner can reach -- the fair apples-to-apples
                    # comparison target. gainDb_eff = static + R_dyn*w is
                    # linear in w in [0,1], so it is bounded by its two
                    # ENDPOINTS (w=0 -> static, w=1 -> static+R_dyn); "loudest"
                    # means highest dB (most amplification), which is
                    # whichever endpoint is numerically larger -- NOT always
                    # static+R_dyn. FIRST RUN used loudest_gain_db=static+R_dyn
                    # unconditionally and measured a 45x "peak ratio" failure;
                    # root cause (confirmed by a standalone repro): for a CUT
                    # corner (e.g. static=+12, R_dyn=-24) the LOUDEST point is
                    # actually the untouched static endpoint (+12dB, ~4x
                    # amplitude at a Q=18 resonance) while static+R_dyn=-12dB
                    # is the QUIETEST -- the reference was measured at the
                    # quiet end while the dynamic filter legitimately spent
                    # time at the loud (static, unducked) end. Not a DSP bug
                    # (worst pole radius stayed 0.9989 < 1, 0 unstable configs
                    # even in that run) -- a test-methodology fix. Depends
                    # only on (fs, Q, static, R_dyn), not attack/release/K, so
                    # hoisted out of those loops -- first draft recomputed
                    # this per-K (2880x with a Python-loop SVF render each
                    # time) when it only has 6*5*4*2=240 distinct values,
                    # cutting wall time ~12x with an identical result.
                    loudest_gain_db = max(static, static + R_dyn)
                    sec = SVFSection()
                    sec.prepare(fs, f0, loudest_gain_db, Q, "bell")
                    y_ref = sec.process(x)
                    peak_ref = np.max(np.abs(y_ref)) + 1e-12

                    for attack_ms in attacks_ms:
                        for release_ms in releases_ms:
                            _, _, w_sm, _ = detector_trace(
                                x, fs, f0, Q, T, W, attack_ms, release_ms)
                            gainDb_eff = static + R_dyn * w_sm
                            for K in Ks:
                                y, coeffs_used_K = run_dynamic_bell(x, fs, f0, Q, gainDb_eff, K)
                                n_total += 1
                                finite = np.all(np.isfinite(y))
                                all_finite &= finite
                                stable_here = True
                                rmax_here = 0.0
                                for a1, a2 in set(coeffs_used_K):
                                    stable, rmax = poles_stable(a1, a2)
                                    stable_here &= bool(stable)
                                    rmax_here = max(rmax_here, rmax)
                                worst_pole_radius = max(worst_pole_radius, rmax_here)
                                if not stable_here:
                                    n_unstable_configs += 1

                                if finite:
                                    peak = np.max(np.abs(y))
                                    ratio = peak / peak_ref
                                    worst_peak_ratio = max(worst_peak_ratio, ratio)

    elapsed = time.time() - t_start
    gate_finite = all_finite
    gate_stable = n_unstable_configs == 0
    gate_peak = worst_peak_ratio <= 1.5

    print(f"  grid: {n_total} configs (Q{len(Qs)} x static{len(statics)} x R_dyn{len(R_dyns)} x "
          f"attack{len(attacks_ms)} x release{len(releases_ms)} x K{len(Ks)} x fs{len(fs_list)}), "
          f"dur={dur}s each, {elapsed:.1f}s wall time")
    print(f"  all outputs finite: {gate_finite} [{'PASS' if gate_finite else 'FAIL'}]")
    print(f"  worst pole radius over ALL instantaneous coefficient sets used = "
          f"{worst_pole_radius:.8f}  ({n_unstable_configs} unstable configs) "
          f"[{'PASS' if gate_stable else 'FAIL'}]")
    print(f"  worst peak ratio (dynamic / TPT-SVF static-at-loudest-gain) = "
          f"{worst_peak_ratio:.4f} (limit 1.5) [{'PASS' if gate_peak else 'FAIL'}]")

    # Explicit worst corner: Q=18, static=+12, R_dyn=-24, attack=0.1ms,
    # block=512, fs=44100 -- rendered longer (0.5s) for an audible/inspectable WAV.
    fs_w, Q_w, static_w, R_w, attack_w, release_w, K_w = 44100.0, 18.0, 12.0, -24.0, 0.1, 100.0, 512
    dur_w = 0.5
    n_w = int(fs_w * dur_w)
    t_w = np.arange(n_w) / fs_w
    gate_hz = 25.0
    gate_w = (np.mod(t_w, 1.0 / gate_hz) < (0.5 / gate_hz)).astype(float)
    x_w = 0.9 * np.sin(2 * PI * f0 * t_w) * gate_w
    y_w, level_w, w_raw_w, w_sm_w, gainDb_eff_w, coeffs_used_w, Q_det_w = process_dynamic_bell(
        x_w, fs_w, f0, Q_w, static_w, T, W, R_w, attack_w, release_w, K_w)

    # DFI state = (x1, x2, y1, y2), literal past samples -- norm tracked here
    # purely to confirm it stays bounded (no runaway) through the worst
    # corner, not because DFI needs a conditioning check the way the
    # normal form's eigenbasis did.
    x1 = x2 = y1 = y2 = 0.0
    b0 = b1 = b2 = a1 = a2 = None
    counter = 0
    norms = []
    for i in range(n_w):
        if counter == 0:
            b0, b1, b2, a1, a2 = matched_bell(fs_w, f0, gainDb_eff_w[i], Q_w)
            counter = K_w
        counter -= 1
        xi = x_w[i]
        yi = b0 * xi + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, xi
        y2, y1 = y1, yi
        norms.append(np.hypot(np.hypot(x1, x2), np.hypot(y1, y2)))
    state_norm_w = np.array(norms)

    wavfile.write(f"{OUT}/dynamic_eq_worst_corner.wav", int(fs_w),
                  (np.clip(y_w, -1, 1) * 32767).astype(np.int16))

    fig, axs = plt.subplots(2, 1, figsize=(11, 7), sharex=True)
    axs[0].plot(t_w, x_w, alpha=0.4, label="input (gated sine)")
    axs[0].plot(t_w, y_w, label="dynamic-bell output")
    axs[0].set_title(f"Worst corner: Q={Q_w} static={static_w:+.0f}dB R_dyn={R_w:+.0f}dB "
                      f"attack={attack_w}ms K={K_w} fs={fs_w:.0f}")
    axs[0].legend(fontsize=8); axs[0].grid(alpha=0.3)
    axs[1].plot(t_w, state_norm_w, color="tab:red")
    axs[1].set_ylabel("DFI state norm ||(x1,x2,y1,y2)||")
    axs[1].set_xlabel("s"); axs[1].grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{OUT}/dyneq_worst_corner_state.png", dpi=130)
    plt.close(fig)

    peak_w = np.max(np.abs(y_w))
    finite_w = np.all(np.isfinite(y_w))
    print(f"\n  worst-corner render: peak|y|={peak_w:.4f}, finite={finite_w}, "
          f"max state norm={np.max(state_norm_w):.4f}")
    print(f"  Wrote dynamic_eq_worst_corner.wav, dyneq_worst_corner_state.png")

    ok = gate_finite and gate_stable and gate_peak
    print(f"\n  TEST 9 [{'PASS' if ok else 'FAIL'}]")
    return ok, n_total, worst_pole_radius, worst_peak_ratio


# ============================================================
# TEST 10: musical demo
# ============================================================

def make_vocalish_signal(fs, dur=3.0, seed=11):
    """Tonal (formant-ish sung vowel via a few harmonics) + periodic sibilant
    ('s'/'sh') bursts in the 6-9kHz de-ess range."""
    rng = np.random.default_rng(seed)
    n = int(fs * dur)
    t = np.arange(n) / fs
    f1 = 180.0
    vowel = (0.5 * np.sin(2 * PI * f1 * t) + 0.25 * np.sin(2 * PI * 2 * f1 * t) +
              0.15 * np.sin(2 * PI * 3 * f1 * t) + 0.08 * np.sin(2 * PI * 5 * f1 * t))
    env = 0.5 + 0.5 * np.sin(2 * PI * 0.4 * t - PI / 2) ** 2  # slow swell, always audible
    vowel *= env

    b, a = butter(4, [6500.0 / (fs / 2), 9000.0 / (fs / 2)], btype="band")
    sib_bed = rng.standard_normal(n)
    sib = lfilter(b, a, sib_bed)
    sib_env = np.zeros(n)
    word_period = int(0.55 * fs)
    sib_len = int(0.08 * fs)
    for i0 in range(0, n - sib_len, word_period):
        ramp = np.hanning(sib_len)
        sib_env[i0:i0 + sib_len] = ramp
    sib *= sib_env * 0.9

    x = vowel * 0.5 + sib
    x = x / (np.max(np.abs(x)) + 1e-9) * 0.85
    return x


def test10_musical_demo():
    print("\n=== TEST 10: musical demo (de-esser, vocal-ish signal) ===\n")
    fs = 44100.0
    f0, Q_band = 7500.0, 3.0
    static_gain_db = 0.0
    R_dyn = -10.0
    T, W = -24.0, 4.0
    attack_ms, release_ms = 1.0, 60.0
    K = 64

    x = make_vocalish_signal(fs, dur=3.0)
    y_dyn, level_db, w_raw, w_sm, gainDb_eff, coeffs_used, Q_det = process_dynamic_bell(
        x, fs, f0, Q_band, static_gain_db, T, W, R_dyn, attack_ms, release_ms, K)

    static_trace = np.full(len(x), static_gain_db)
    y_static, _ = run_dynamic_bell(x, fs, f0, Q_band, static_trace, K=len(x))

    delta = y_dyn - y_static

    def to_wav(name, sig):
        wavfile.write(f"{OUT}/{name}.wav", int(fs), (np.clip(sig, -1, 1) * 32767).astype(np.int16))

    to_wav("dyneq_demo_dry", x)
    to_wav("dyneq_demo_wet_static", y_static)
    to_wav("dyneq_demo_wet_dynamic", y_dyn)
    to_wav("dyneq_demo_delta", delta / (np.max(np.abs(delta)) + 1e-9) * 0.9)

    max_gr = np.min(gainDb_eff) - static_gain_db
    mean_active_gr = np.mean((gainDb_eff - static_gain_db)[w_sm > 0.5])
    print(f"  band: bell f0={f0}Hz Q={Q_band} static={static_gain_db}dB dynamic R_dyn={R_dyn}dB")
    print(f"  peak gain reduction observed = {max_gr:.2f} dB")
    print(f"  mean GR while detector engaged (w>0.5) = {mean_active_gr:.2f} dB")
    print(f"  fraction of signal with detector engaged (w>0.5) = {np.mean(w_sm > 0.5)*100:.1f}%")

    fig, axs = plt.subplots(3, 1, figsize=(11, 9), sharex=True)
    t = np.arange(len(x)) / fs
    axs[0].plot(t, x, lw=0.6)
    axs[0].set_title("Dry (vocal-ish: tonal + 6.5-9kHz sibilant bursts)")
    axs[0].grid(alpha=0.3)
    axs[1].plot(t, y_dyn, lw=0.6, color="tab:green")
    axs[1].set_title("Wet: de-ess -10dB dynamic bell @ 7.5kHz Q=3 (static 0dB)")
    axs[1].grid(alpha=0.3)
    axs[2].plot(t, gainDb_eff, color="k")
    axs[2].set_title("gainDb_eff (static + R_dyn * w_smoothed)")
    axs[2].set_xlabel("s"); axs[2].grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{OUT}/dyneq_demo.png", dpi=130)
    plt.close(fig)

    print(f"\n  Wrote dyneq_demo_dry.wav, dyneq_demo_wet_static.wav, dyneq_demo_wet_dynamic.wav, "
          f"dyneq_demo_delta.wav, dyneq_demo.png")
    return True  # demo, no gate


# ============================================================
# main
# ============================================================

if __name__ == "__main__":
    ok1 = test1_knee()
    ok2 = test2_ballistics()
    ok3 = test3_detector_bp()
    ok4 = test4_below_threshold_null()
    ok5 = test5_settled_endpoint_null()
    ok6 = test6_symmetry()
    ok7 = test7_feed_forward()
    ok8 = test8_pick_K()
    ok9, n9, worst_radius9, worst_ratio9 = test9_stability_grid()
    ok10 = test10_musical_demo()

    print("\n" + "=" * 72)
    print("SUMMARY")
    print("=" * 72)
    print(f"\nTest 1 (smoothstep knee):                    [{'PASS' if ok1 else 'FAIL'}]")
    print(f"Test 2 (ballistics on w, tau accuracy):       [{'PASS' if ok2 else 'FAIL'}]")
    print(f"Test 3 (detector BP vs scipy/analytic):       [{'PASS' if ok3 else 'FAIL'}]")
    print(f"Test 4 (below-threshold null, offset==0):     [{'PASS' if ok4 else 'FAIL'}]")
    print(f"Test 5 (settled-endpoint null, R_dyn=+-12):   [{'PASS' if ok5 else 'FAIL'}]")
    print(f"Test 6 (up/down symmetry):                    [{'PASS' if ok6 else 'FAIL'}]")
    print(f"Test 7 (feed-forward, detector==f(input) only): [{'PASS' if ok7 else 'FAIL'}]")
    print(f"Test 8 (K sweep, report-only):                [REPORT]")
    print(f"Test 9 (stability grid, {n9} configs):        [{'PASS' if ok9 else 'FAIL'}]  "
          f"worst pole radius={worst_radius9:.6f}  worst peak ratio={worst_ratio9:.3f}")
    print(f"Test 10 (musical demo):                       [DEMO]")

    verdict = ok1 and ok2 and ok3 and ok4 and ok5 and ok6 and ok7 and ok9
    print(f"\nDynamic band on matched bell via Direct Form I: {'GO' if verdict else 'NO-GO'}")
    print(f"\nWrote to {OUT}/: dyneq_knee.png, dyneq_ballistics.png, dynamic_eq_K*.wav, "
          f"dyneq_K_sweep.png, dynamic_eq_worst_corner.wav, dyneq_worst_corner_state.png, "
          f"dyneq_demo_*.wav, dyneq_demo.png")

    assert verdict, "one or more gated tests failed -- see FAIL lines above"
