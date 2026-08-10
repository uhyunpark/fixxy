"""Dump spectral-processor reference fixtures from the VERIFIED proto pipeline
into test/spectral_fixtures.h (consumed by test/spectral_node_test.cpp).

Run from repo root:  ./proto/.venv/bin/python proto/dump_spectral_fixtures.py

WHAT IS PINNED HERE (three fixture groups):

(a) FFT round-trip / known-vector: a couple of length-1024 inputs and their
    numpy rfft (re/im half-spectra, length 513) so the C++ hand-rolled radix-2
    forward FFT matches numpy (~1e-9) and its round trip holds (~1e-12).

(b) Full-chain STREAMING fixtures: the REAL proto chain
    (spectral_core.ProcessingSTFT + spectral_ballistics.SpectralCoreBallistics)
    at fs=44100, N=1024, hop=256, MONO, on four configs (identity, default on a
    burst probe, a depth/detail variation, an attack/release variation). The
    stored output is run_chain_ballistics's INPUT-ALIGNED array (the leading
    N-sample latency already removed, length == len(input)). The C++ test must
    therefore feed the float32 input, pad with zeros past the tail, and read its
    delayed output starting at sample N (= latencySamples) — i.e. cppOut[N:N+L]
    lines up with fixture.out[0:L]. Inputs are float32-rounded (like
    dump_dynamic_fixtures) so the C++ node, which casts its float buffer back to
    double, sees bit-identical samples.

    STEREO-LINK RATIONALE (documented per the port brief): the C++ node's v1
    stereo link is a LINKED COMMON GAIN MAP — detection magnitude = element-wise
    MAX across channels of |spec|, one gain map computed from it and applied to
    BOTH channels, one shared per-bin ballistics state. For a MONO signal that
    max reduces to |spec| itself, so these mono fixtures ARE the reference the
    stereo path must reproduce channel-for-channel when fed L==R (asserted
    C++-side, no separate fixture needed).

(c) The frozen fitted constants, so the C++ test asserts the node's hard-coded
    constants equal the proto's. calibrate() is re-run here and asserted to still
    reproduce the frozen values within 1e-9 (catches scipy drift); the CHAIN is
    then generated from the FROZEN constants (not calibrate()'s raw output) so
    the fixtures and the C++ literals use identical constants and the only
    numeric divergence the full-chain test sees is the radix-2-vs-pocketfft FFT
    rounding.

Numbers are emitted with %.17g and NEVER hand-copied from stdout.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spectral_core as sc                       # noqa: E402
from spectral_ballistics import (                # noqa: E402
    run_chain_ballistics, make_burst_probe, DEFAULT_ATTACK_MS, DEFAULT_RELEASE_MS)

N = 1024
HOP = 256
FS = 44100.0

# ---- FROZEN calibrated constants (must equal SpectralNode's literals) ----
FROZEN = {
    "max_db": 16.167635300821875,
    "knee_c": 18.78301217932193,
    "knee_w": 9.010408026990198,
    "w0": 0.07788596060802633,
    "w5": 0.08630905922389523,
    "w10": 0.005,
    "m0": 0.2719078891498013,
    "m10": 0.9067617761812925,
    "env_halfwidth_oct": 0.5,
}


def assert_calibration_reproduces():
    """Re-run the verified calibration and assert it still lands on the frozen
    numbers (scipy drift guard). A failure here is a real signal, not a tolerance
    to loosen (proto/CLAUDE.md)."""
    cal = sc.calibrate()
    for key, frozen in FROZEN.items():
        got = float(cal[key])
        assert abs(got - frozen) < 1e-9, \
            f"calibrate() drifted on {key}: got {got!r}, frozen {frozen!r}"
    print("calibrate() reproduces the frozen constants within 1e-9 (no drift).")


def frozen_params():
    """The proto PARAMS dict built from the FROZEN constants (identical to the
    C++ literals) so fixtures and C++ share constants exactly."""
    return {
        "env_halfwidth_oct": FROZEN["env_halfwidth_oct"],
        "max_db": FROZEN["max_db"],
        "knee_c": FROZEN["knee_c"],
        "knee_w": FROZEN["knee_w"],
        "w0": FROZEN["w0"], "w5": FROZEN["w5"], "w10": FROZEN["w10"],
        "m0": FROZEN["m0"], "m10": FROZEN["m10"],
    }


def f32(x):
    """Round to float32 then back to float64 — exactly what the C++ node sees
    when it casts the float buffer sample to double."""
    return np.asarray(x, dtype=np.float32).astype(np.float64)


def arr(a):
    return ", ".join("%.17g" % v for v in a)


# ============================================================================
# (a) FFT fixtures
# ============================================================================
def fft_fixtures():
    rng = np.random.default_rng(4)
    t = np.arange(N) / FS
    inputs = {
        # a deterministic mixed-tone frame (bin-centred + off-bin partials).
        "tones": (0.6 * np.sin(2 * np.pi * (10 * FS / N) * t)
                  + 0.3 * np.sin(2 * np.pi * 3300.0 * t)
                  + 0.1 * np.cos(2 * np.pi * 12000.0 * t)),
        # a random frame (broadband, exercises every bin).
        "noise": rng.standard_normal(N) * 0.3,
    }
    fixtures = []
    for name, x in inputs.items():
        x = np.asarray(x, dtype=np.float64)
        X = np.fft.rfft(x)  # forward, unnormalised — the C++ forward target
        fixtures.append((name, x, X.real.copy(), X.imag.copy()))
    return fixtures


# ============================================================================
# (b) Full-chain streaming fixtures
# ============================================================================
def chain_fixtures(params):
    fixtures = []

    # 1) depth=0 identity on a short noise burst.
    rng = np.random.default_rng(11)
    x_id = rng.standard_normal(2048) * 0.3
    fixtures.append(("identity_noise", 0.0, 5.0, DEFAULT_ATTACK_MS, DEFAULT_RELEASE_MS, f32(x_id)))

    # 2) default config on the burst probe (short so the header stays small).
    #    make_burst_probe with tiny on/off keeps it ~2000 samples while still
    #    exercising the tone onset (attack) and the gap (release).
    burst, _ = make_burst_probe(dur_on=0.02, dur_off=0.025, cycles=1,
                                tone_rms_dbfs=-12.0, bed_rms_dbfs=-50.0, fs=FS)
    fixtures.append(("default_burst", 5.0, 5.0, DEFAULT_ATTACK_MS, DEFAULT_RELEASE_MS, f32(burst)))

    # 3) depth/detail variation on a resonance-in-noise step.
    x_res = sc.make_resonance_step(12.0, dur=2048 / FS)
    fixtures.append(("depth8_detail2", 8.0, 2.0, DEFAULT_ATTACK_MS, DEFAULT_RELEASE_MS, f32(x_res)))

    # 4) attack/release variation on a tone burst.
    burst2, _ = make_burst_probe(dur_on=0.02, dur_off=0.025, cycles=1,
                                 tone_rms_dbfs=-10.0, bed_rms_dbfs=-48.0, fs=FS)
    fixtures.append(("fast_atk_slow_rel", 5.0, 5.0, 5.0, 200.0, f32(burst2)))

    out = []
    for name, depth, detail, atk, rel, x in fixtures:
        y = run_chain_ballistics(x, params, depth=depth, detail=detail,
                                 attack_ms=atk, release_ms=rel)  # input-aligned, len == len(x)
        out.append((name, depth, detail, atk, rel, x, np.asarray(y, dtype=np.float64)))
    return out


def main():
    assert_calibration_reproduces()
    params = frozen_params()

    ffts = fft_fixtures()
    chains = chain_fixtures(params)

    L = []
    L.append("// AUTO-GENERATED by proto/dump_spectral_fixtures.py -- DO NOT EDIT.")
    L.append("// Spectral-processor reference fixtures from the VERIFIED proto pipeline")
    L.append("// (spectral_stft / spectral_core / spectral_ballistics). See the dump")
    L.append("// script header for the streaming alignment (cppOut[N:N+L] vs out[0:L])")
    L.append("// and the linked-common-gain stereo rationale.")
    L.append("#pragma once")
    L.append("#include <array>\n")
    L.append("namespace specfix {\n")
    L.append("inline constexpr int kN = %d;" % N)
    L.append("inline constexpr int kHop = %d;" % HOP)
    L.append("inline constexpr double kFs = %.17g;\n" % FS)

    # (c) frozen constants
    L.append("// Frozen calibrated constants (C++ node literals must equal these).")
    for key in ("max_db", "knee_c", "knee_w", "w0", "w5", "w10", "m0", "m10",
                "env_halfwidth_oct"):
        L.append("inline constexpr double k_%s = %.17g;" % (key, FROZEN[key]))
    L.append("")

    # (a) FFT fixtures
    L.append("struct FftFixture {")
    L.append("    const char* name;")
    L.append("    std::array<double, kN> x;")
    L.append("    std::array<double, kN / 2 + 1> re;")
    L.append("    std::array<double, kN / 2 + 1> im;")
    L.append("};")
    L.append("inline const FftFixture kFftFixtures[] = {")
    for name, x, re, im in ffts:
        L.append('    {"%s",' % name)
        L.append("     {{%s}}," % arr(x))
        L.append("     {{%s}}," % arr(re))
        L.append("     {{%s}}}," % arr(im))
    L.append("};")
    L.append("inline constexpr int kNumFftFixtures = %d;\n" % len(ffts))

    # (b) chain fixtures
    maxlen = max(len(x) for *_, x, _y in chains)
    L.append("inline constexpr int kMaxChainLen = %d;" % maxlen)
    L.append("struct ChainFixture {")
    L.append("    const char* name;")
    L.append("    double depth, detail, attackMs, releaseMs;")
    L.append("    int len;")
    L.append("    std::array<double, kMaxChainLen> x;    // float32-rounded input")
    L.append("    std::array<double, kMaxChainLen> out;  // proto output, input-aligned")
    L.append("};")
    L.append("inline const ChainFixture kChainFixtures[] = {")
    for name, depth, detail, atk, rel, x, y in chains:
        n = len(x)
        xp = list(x) + [0.0] * (maxlen - n)
        yp = list(y) + [0.0] * (maxlen - n)
        L.append('    {"%s", %.17g, %.17g, %.17g, %.17g, %d,'
                  % (name, depth, detail, atk, rel, n))
        L.append("     {{%s}}," % arr(xp))
        L.append("     {{%s}}}," % arr(yp))
    L.append("};")
    L.append("inline constexpr int kNumChainFixtures = %d;\n" % len(chains))
    L.append("}  // namespace specfix")

    out_path = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "test", "spectral_fixtures.h"))
    with open(out_path, "w") as fp:
        fp.write("\n".join(L) + "\n")
    print("Wrote %s: %d FFT fixtures, %d chain fixtures (max len %d)"
          % (out_path, len(ffts), len(chains), maxlen))


if __name__ == "__main__":
    main()
