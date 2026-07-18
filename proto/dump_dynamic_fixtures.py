"""Dump dynamic-EQ reference fixtures from the VERIFIED proto pipeline into a
C++ header consumed by test/dynamic_eq_test.cpp.

WHY THIS ISN'T JUST dynamic_eq.py's numbers (a genuine reconciliation, read it):
proto/dynamic_eq.py imports matched_bell / matched_bandpass from
proto/matched_filters.py -- the VICANEK matched design. But the SHIPPING C++
kernel (src/dsp/MatchedCoeffs.h, matched::computeBiquad) is the IVANTSOV
phi-mapped design (proto/matched_kernel.py, ivantsov_2nd). They are DIFFERENT
filter designs with DIFFERENT coefficients. dynamic_eq.py's PASS/FAIL gates are
all INVARIANT to which matched bell is used -- test 4/5 compare dynamic-vs-static
with the SAME bell function on both sides, test 9 grades against a TPT-SVF
reference with a 1.5x tolerance -- so Vicanek was an incidental reuse, not a
verified requirement. The C++ port MUST use Ivantsov for BOTH the band and the
detector bandpass, or (a) dynEnabled=false would not be bit-exact to the shipping
static path (which is Ivantsov) and (b) two matched-bell designs would fight in
one node. So these fixtures run the proto's VERIFIED, design-independent pipeline
pieces -- smoothstep_knee and run_ballistics, imported unchanged from
dynamic_eq.py -- but substitute ivantsov_2nd for the bandpass detector and the
bell, exactly as the C++ does. The offsetDb trajectory (= range * w_smoothed)
does not depend on the bell at all (only the detector BP + knee + ballistics),
and the settled coefficients are ivantsov_2nd at the settled gain -- both
reproduced by C++ to the tolerances the test asserts (1e-9 / 1e-12).

The detector bandpass is realized in DIRECT FORM I here (not scipy lfilter's
transposed DF2) to match the C++ kernel's realization bit-for-bit -- the filter
is the same transfer function either way (the detector's own coefficients never
change mid-run), this just removes realization rounding as a variable so the
1e-9 gate is clean.

Run from repo root:  ./proto/.venv/bin/python proto/dump_dynamic_fixtures.py
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from matched_kernel import ivantsov_2nd, ivantsov_tilt  # noqa: E402  (Ivantsov = shipping design)
from compressor import tau_to_alpha  # noqa: E402  (verified tau->alpha, reused verbatim)
from dynamic_eq import smoothstep_knee  # noqa: E402  (verified knee, reused verbatim)

SIGMA = 2.0283139124815617  # matched_kernel proto test2 recommended sigma (grid index 38)
KNEE_W = 6.0                # internal fixed knee width (proto dynamic tests use W=6)

ORD = {"bell": 0, "low_shelf": 1, "high_shelf": 2, "tilt": 3, "notch": 4, "bandpass": 5}

# (name, type, fs, f0, Q, staticGainDb, rangeDb, threshDb, attackMs, releaseMs)
# spans range +/-, thresholds, fast/slow attack+release, fs 44.1/48/96, the
# gain-bearing types, and -- CRITICAL -- the deep-cut real-pole crossing corner
# (Q=18 static +12 range -24: gain sweeps +12 -> -12, poles pass through critical
# damping, the exact case that broke the old normal form; DF1 must stay finite).
CONFIGS = [
    ("bell_deess",     "bell",       44100.0, 3000.0,  2.0,   0.0, -12.0, -24.0, 2.0,  80.0),
    ("bell_up",        "bell",       48000.0, 3000.0,  2.0,   0.0,  12.0, -24.0, 2.0,  80.0),
    ("bell_fast96",    "bell",       96000.0, 5000.0,  3.0,   0.0, -18.0, -30.0, 0.5,  50.0),
    ("bell_veryfast",  "bell",       44100.0, 8000.0,  4.0,   0.0, -12.0, -20.0, 0.1,  20.0),
    ("lowshelf_up",    "low_shelf",  48000.0,  200.0,  0.7071, 3.0,  6.0, -24.0, 5.0, 120.0),
    ("highshelf_down", "high_shelf", 44100.0, 6000.0,  0.7071, 0.0, -9.0, -24.0, 2.0,  80.0),
    ("tilt_up",        "tilt",       48000.0, 1000.0,  0.7071, 0.0,  6.0, -24.0, 2.0,  80.0),
    ("bell_boost96",   "bell",       96000.0, 2000.0,  6.0,  -6.0, 12.0, -30.0, 1.0, 100.0),
    ("bell_worstcorner","bell",      44100.0, 5000.0, 18.0,  12.0, -24.0, -28.0, 0.1, 100.0),
    ("bell_bigboost",  "bell",       48000.0, 1000.0,  1.0,   0.0, 24.0, -24.0, 3.0,  60.0),
]

N_TRAJ = 1024  # per-config trajectory length (long enough to exercise attack rise)


def bell_coeffs(bt, fs, f0, Q, g):
    if bt == "tilt":
        return ivantsov_tilt(fs, f0, Q, g, SIGMA)
    return ivantsov_2nd(bt, fs, f0, Q, g, SIGMA)


def gated_sine(fs, f0, n, amp=0.5, gate_hz=25.0):
    """Deterministic drive: a tone at the band centre gated on/off at 25 Hz, so
    the detector demand sweeps 0 -> 1 -> 0 within the window (rise = attack,
    fall = release). Rounded to float32 because the real node processes a float
    buffer -- storing the already-rounded value makes the C++ detector (which
    casts the float buffer sample back to double) bit-identical to this."""
    t = np.arange(n) / fs
    gate = (np.mod(t, 1.0 / gate_hz) < (0.5 / gate_hz)).astype(float)
    x = amp * np.sin(2.0 * np.pi * f0 * t) * gate
    return x.astype(np.float32).astype(np.float64)


def detector_df1(x, fs, f0, Q_band, threshDb, attackMs, releaseMs):
    """Feed-forward detector, DIRECT FORM I bandpass (Ivantsov), smoothstep knee,
    smooth_decoupled_rtt ballistics -- a verbatim mirror of the C++
    SvfBandNode::processMatchedDynamic detector. Returns w_smoothed[n]."""
    q_det = float(np.clip(Q_band, 0.5, 4.0))
    b0, b1, b2, a1, a2 = ivantsov_2nd("bandpass", fs, f0, q_det, 0.0, SIGMA)
    aA = tau_to_alpha(attackMs / 1000.0, fs)
    aR = tau_to_alpha(releaseMs / 1000.0, fs)
    knee_lo = threshDb - KNEE_W / 2.0
    inv_w = 1.0 / KNEE_W

    n = len(x)
    w = np.empty(n)
    x1 = x2 = y1 = y2 = 0.0
    s1 = 0.0
    yb = 0.0
    for i in range(n):
        sc = abs(x[i])  # single channel: max-abs over 1 ch == |x|
        bp = b0 * sc + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, sc
        y2, y1 = y1, bp
        mag = abs(bp)
        level = 20.0 * np.log10(mag if mag > 1e-6 else 1e-6)
        u = (level - knee_lo) * inv_w
        u = 0.0 if u < 0.0 else (1.0 if u > 1.0 else u)
        w_raw = u * u * (3.0 - 2.0 * u)
        if w_raw >= s1:
            s1 = w_raw
        else:
            s1 = aR * (s1 - w_raw) + w_raw
        yb = aA * (yb - s1) + s1
        w[i] = yb
    return w


def main():
    lines = []
    lines.append("// AUTO-GENERATED by proto/dump_dynamic_fixtures.py -- DO NOT EDIT.")
    lines.append("// Dynamic-EQ reference trajectories + settled coefficients from the")
    lines.append("// VERIFIED proto pipeline (Ivantsov design; see the dump script header).")
    lines.append("#pragma once")
    lines.append("#include <array>\n")
    lines.append("namespace dynfix {\n")
    lines.append("inline constexpr double kSigma = %.17g;" % SIGMA)
    lines.append("inline constexpr double kKneeW = %.17g;" % KNEE_W)
    lines.append("inline constexpr int kNTraj = %d;\n" % N_TRAJ)
    lines.append("struct DynFixture {")
    lines.append("    const char* name;")
    lines.append("    int typeOrdinal;")
    lines.append("    double fs, f0, q, staticGainDb, rangeDb, threshDb, attackMs, releaseMs;")
    lines.append("    std::array<double, kNTraj> x;         // float32-rounded input")
    lines.append("    std::array<double, kNTraj> w;         // detector w_smoothed")
    lines.append("    std::array<double, kNTraj> offsetDb;  // rangeDb * w")
    lines.append("    double sb0, sb1, sb2, sa1, sa2;       // settled bell coeffs @ gain+range")
    lines.append("};\n")

    entries = []
    for name, bt, fs, f0, Q, sg, rng, th, att, rel in CONFIGS:
        x = gated_sine(fs, f0, N_TRAJ)
        w = detector_df1(x, fs, f0, Q, th, att, rel)
        offset = rng * w
        sb0, sb1, sb2, sa1, sa2 = bell_coeffs(bt, fs, f0, Q, sg + rng)

        def arr(a):
            return ", ".join("%.17g" % v for v in a)

        entries.append(
            '    {"%s", %d, %.17g, %.17g, %.17g, %.17g, %.17g, %.17g, %.17g, %.17g,\n'
            "     {{%s}},\n"
            "     {{%s}},\n"
            "     {{%s}},\n"
            "     %.17g, %.17g, %.17g, %.17g, %.17g}," % (
                name, ORD[bt], fs, f0, Q, sg, rng, th, att, rel,
                arr(x), arr(w), arr(offset),
                sb0, sb1, sb2, sa1, sa2))

    lines.append("inline const DynFixture kFixtures[] = {")
    lines.extend(entries)
    lines.append("};")
    lines.append("inline constexpr int kNumFixtures = %d;" % len(entries))
    lines.append("\n}  // namespace dynfix")

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "test", "dynamic_eq_fixtures.h")
    out = os.path.normpath(out)
    with open(out, "w") as fp:
        fp.write("\n".join(lines) + "\n")
    print("Wrote %s: %d fixtures, %d-sample trajectories" % (out, len(entries), N_TRAJ))


if __name__ == "__main__":
    main()
