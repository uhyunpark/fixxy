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

(d) WEIGHT-CURVE fixtures (proto/spectral_weight.py, verified against
    src/dsp/BandResponse.h's recipe + src/dsp/SpectralWeight.h's gate formula):
    three configs -- BOOST (Bell +12dB @3k q0.7), SUPPRESS (Bell -24dB @3k
    q0.7), GATE (bands off, lo=200/hi=8000) -- run through
    SpectralCoreWeightedBallistics (weight injected before the tanh knee,
    frequency smoothing, THEN time ballistics -- same layering as (b), the
    shipping node's actual pipeline once the weight math is wired in). Same
    input construction and alignment convention as (b): fixed-seed noise + 3kHz
    resonance (spectral_core.make_resonance_step), float32-rounded, output is
    input-aligned (leading N-sample latency dropped). WeightBand fields mirror
    src/dsp/SpectralWeight.h::WeightBand exactly (on/type/freqHz/gainDb/q);
    unused band slots (up to kMaxWeightBands=8) are zero-filled with on=false.

(e) SIDEBAR fixtures (proto/spectral_sidebar.py, verified against the frozen
    7-param spec: max_cut_db/wet_trim_db (scale), ms_mode/link_pct/focus
    (stereo), tilt_attack/tilt_release/tilt_detail (tilt)) -- three STEREO
    configs run through spectral_sidebar.run_sidebar at the FROZEN constants
    + default depth/detail/attack/release (matching (b)/(d)'s "default"
    config): (a) "sidebar_link0_left_only" -- link=0, a resonance in the
    left channel only, right channel a quiet independent noise floor (the
    left-only-cut / right-untouched case spectral_sidebar.gate5 asserts);
    (b) "sidebar_ms_s_only" -- ms_mode on, an S-only resonance (shared
    noise floor -> pure M, L=+tone/R=-tone -> pure S), link=0 (isolating the
    M/S transform from the separately-gated link mechanism, same resolution
    spectral_sidebar.gate4 uses -- see that gate's docstring); (c)
    "sidebar_kitchen_sink" -- max_cut_db=6, wet_trim_db=-3, link_pct=50,
    focus=+0.5, tilt_attack=+0.5, tilt_detail=-0.5 (tilt_release left at 0),
    on a generic stereo noise+resonance input. Same float32-input-rounding /
    input-aligned-output convention as (b)/(d). Weight bands are unused here
    (bands=[], lo=20/hi=20000) -- SIDEBAR is orthogonal to the weight curve.

Numbers are emitted with %.17g and NEVER hand-copied from stdout.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spectral_core as sc                       # noqa: E402
from spectral_ballistics import (                # noqa: E402
    run_chain_ballistics, make_burst_probe, DEFAULT_ATTACK_MS, DEFAULT_RELEASE_MS)
import spectral_weight as sw                     # noqa: E402
from spectral_weight import run_chain_weighted_ballistics, BandType  # noqa: E402
import spectral_sidebar as ss                    # noqa: E402
from spectral_sidebar import run_sidebar         # noqa: E402

kMaxWeightBands = 8  # mirrors spectralweight::kMaxWeightBands (src/dsp/SpectralWeight.h)

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


def _wband(on, band_type, freq_hz, gain_db, q):
    """One WeightBand tuple in src/dsp/SpectralWeight.h field order
    (on, type, freqHz, gainDb, q)."""
    return (on, band_type, freq_hz, gain_db, q)


def weight_chain_fixtures(params):
    """The 3 weight-curve fixture sets (proto/spectral_weight.py's BOOST/
    SUPPRESS/GATE cases -- see that file's test5/test6/test7 for the verified
    numbers and why this specific rel_db and these band configs), run through
    the full weighted+ballistics chain at default depth/detail/attack/release
    (matching (b)'s "default_burst" config). Each fixture's `bands` list is
    padded to kMaxWeightBands with off/zeroed slots."""
    rel_db = sw._BOOST_SUPPRESS_REL_DB  # -6.0: single source of truth with the proto's own asserts
    x = f32(sc.make_resonance_step(rel_db, dur=2048 / FS))  # round FIRST, like (b)'s fixtures

    def pad(bands):
        return bands + [_wband(False, 0, 1000.0, 0.0, 0.7)] * (kMaxWeightBands - len(bands))

    configs = [
        ("weight_boost_bell3k", pad([_wband(True, BandType.Bell, 3000.0, 12.0, 0.7)]),
         20.0, 20000.0),
        ("weight_suppress_bell3k", pad([_wband(True, BandType.Bell, 3000.0, -24.0, 0.7)]),
         20.0, 20000.0),
        ("weight_gate_200_8000", pad([]), 200.0, 8000.0),
    ]

    out = []
    for name, bands, lo, hi in configs:
        band_dicts = [dict(on=on, type=t, freq_hz=f, gain_db=g, q=q)
                      for (on, t, f, g, q) in bands if on]
        y = run_chain_weighted_ballistics(
            x, params, depth=5.0, detail=5.0,
            attack_ms=DEFAULT_ATTACK_MS, release_ms=DEFAULT_RELEASE_MS,
            bands=band_dicts, lo=lo, hi=hi)
        out.append((name, bands, lo, hi, x, np.asarray(y, dtype=np.float64)))
    return out


def sidebar_fixtures(params):
    """The 3 SIDEBAR fixture sets (proto/spectral_sidebar.py -- see that
    file's gate2/gate4/gate5 for the verified per-assert numbers behind
    these configs), run through run_sidebar at the FROZEN constants +
    default depth/detail/attack/release. Each tuple:
    (name, xL, xR, outL, outR, maxCutDb, wetTrimDb, msMode, linkPct, focus,
     tiltAttack, tiltRelease, tiltDetail)."""
    out = []

    # (a) link=0, left-only resonance: right channel must come out untouched
    # (gain ~= 1) while left is cut -- spectral_sidebar.gate5's construction.
    dur_a = 2048 / FS
    tone = sc._sine_rms(sc.NOTCH_F0, dur_a, -14.0)
    floor_l = sc._noise_rms(dur_a, -36.0)[:len(tone)]
    xL_a = f32(tone + floor_l)
    xR_a = f32(sc._noise_rms(dur_a, -50.0)[:len(tone)])
    outL_a, outR_a = run_sidebar(xL_a, xR_a, params, depth=5.0, detail=5.0,
                                  attack_ms=DEFAULT_ATTACK_MS, release_ms=DEFAULT_RELEASE_MS,
                                  hop=HOP, N_=N, fs=FS, bands=[], lo=20.0, hi=20000.0,
                                  max_cut_db=24.0, wet_trim_db=0.0, ms_mode=False,
                                  link_pct=0.0, focus=0.0,
                                  tilt_attack=0.0, tilt_release=0.0, tilt_detail=0.0)
    out.append(("sidebar_link0_left_only", xL_a, xR_a,
                np.asarray(outL_a, dtype=np.float64), np.asarray(outR_a, dtype=np.float64),
                24.0, 0.0, False, 0.0, 0.0, 0.0, 0.0, 0.0))

    # (b) M/S: S-only resonance, ms_mode on, link=0 (isolates the M/S
    # transform from the separately-gated link mechanism -- see
    # spectral_sidebar.gate4's docstring for why link=100 is a confound here).
    dur_b = 2048 / FS
    noise = sc._noise_rms(dur_b, -20.0)
    tone_b = sc._sine_rms(sc.NOTCH_F0, dur_b, -14.0)[:len(noise)]
    xL_b = f32(noise + tone_b)
    xR_b = f32(noise - tone_b)
    outL_b, outR_b = run_sidebar(xL_b, xR_b, params, depth=5.0, detail=5.0,
                                  attack_ms=DEFAULT_ATTACK_MS, release_ms=DEFAULT_RELEASE_MS,
                                  hop=HOP, N_=N, fs=FS, bands=[], lo=20.0, hi=20000.0,
                                  max_cut_db=24.0, wet_trim_db=0.0, ms_mode=True,
                                  link_pct=0.0, focus=0.0,
                                  tilt_attack=0.0, tilt_release=0.0, tilt_detail=0.0)
    out.append(("sidebar_ms_s_only", xL_b, xR_b,
                np.asarray(outL_b, dtype=np.float64), np.asarray(outR_b, dtype=np.float64),
                24.0, 0.0, True, 0.0, 0.0, 0.0, 0.0, 0.0))

    # (c) kitchen sink -- all groups engaged at once, generic stereo input.
    dur_c = 2048 / FS
    xL_c = f32(sc.make_resonance_step(12.0, dur=dur_c))
    xR_c = f32(sc.make_resonance_step(12.0, dur=dur_c))
    outL_c, outR_c = run_sidebar(xL_c, xR_c, params, depth=5.0, detail=5.0,
                                  attack_ms=DEFAULT_ATTACK_MS, release_ms=DEFAULT_RELEASE_MS,
                                  hop=HOP, N_=N, fs=FS, bands=[], lo=20.0, hi=20000.0,
                                  max_cut_db=6.0, wet_trim_db=-3.0, ms_mode=False,
                                  link_pct=50.0, focus=0.5,
                                  tilt_attack=0.5, tilt_release=0.0, tilt_detail=-0.5)
    out.append(("sidebar_kitchen_sink", xL_c, xR_c,
                np.asarray(outL_c, dtype=np.float64), np.asarray(outR_c, dtype=np.float64),
                6.0, -3.0, False, 50.0, 0.5, 0.5, 0.0, -0.5))

    return out


def main():
    assert_calibration_reproduces()
    params = frozen_params()

    ffts = fft_fixtures()
    chains = chain_fixtures(params)
    wchains = weight_chain_fixtures(params)
    sidebar = sidebar_fixtures(params)

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

    # (d) weight-curve chain fixtures
    L.append("// Weight-curve fixtures (proto/spectral_weight.py). WeightBand field")
    L.append("// order mirrors src/dsp/SpectralWeight.h::WeightBand exactly.")
    L.append("inline constexpr int kMaxWeightBands = %d;" % kMaxWeightBands)
    L.append("struct WeightBandFixture {")
    L.append("    bool on;")
    L.append("    int type;")
    L.append("    double freqHz, gainDb, q;")
    L.append("};")
    wmaxlen = max(len(x) for *_, x, _y in wchains)
    L.append("inline constexpr int kMaxWeightChainLen = %d;" % wmaxlen)
    L.append("struct WeightChainFixture {")
    L.append("    const char* name;")
    L.append("    std::array<WeightBandFixture, kMaxWeightBands> bands;")
    L.append("    double rangeLoHz, rangeHiHz;")
    L.append("    double depth, detail, attackMs, releaseMs;")
    L.append("    int len;")
    L.append("    std::array<double, kMaxWeightChainLen> x;    // float32-rounded input")
    L.append("    std::array<double, kMaxWeightChainLen> out;  // proto output, input-aligned")
    L.append("};")
    L.append("inline const WeightChainFixture kWeightChainFixtures[] = {")
    for name, bands, lo, hi, x, y in wchains:
        n = len(x)
        xp = list(x) + [0.0] * (wmaxlen - n)
        yp = list(y) + [0.0] * (wmaxlen - n)
        band_strs = ", ".join(
            "{%s, %d, %.17g, %.17g, %.17g}" % (("true" if on else "false"), t, f, g, q)
            for (on, t, f, g, q) in bands)
        L.append('    {"%s",' % name)
        L.append("     {{%s}}," % band_strs)
        L.append("     %.17g, %.17g, 5.0, 5.0, %.17g, %.17g, %d,"
                  % (lo, hi, DEFAULT_ATTACK_MS, DEFAULT_RELEASE_MS, n))
        L.append("     {{%s}}," % arr(xp))
        L.append("     {{%s}}}," % arr(yp))
    L.append("};")
    L.append("inline constexpr int kNumWeightChainFixtures = %d;\n" % len(wchains))

    # (e) SIDEBAR fixtures (proto/spectral_sidebar.py)
    L.append("// SIDEBAR fixtures (proto/spectral_sidebar.py): the 7 new stereo/tilt/scale")
    L.append("// globals layered on top of the weight+ballistics chain. STEREO (2-channel)")
    L.append("// input/output, same float32-input-rounding / input-aligned-output convention")
    L.append("// as the chain/weight-chain fixtures above.")
    smaxlen = max(len(xL) for _, xL, _xR, _oL, _oR, *_ in sidebar)
    L.append("inline constexpr int kMaxSidebarLen = %d;" % smaxlen)
    L.append("struct SidebarFixture {")
    L.append("    const char* name;")
    L.append("    double maxCutDb, wetTrimDb;")
    L.append("    bool msMode;")
    L.append("    double linkPct, focus;")
    L.append("    double tiltAttack, tiltRelease, tiltDetail;")
    L.append("    double depth, detail, attackMs, releaseMs;")
    L.append("    int len;")
    L.append("    std::array<double, kMaxSidebarLen> xL, xR;      // float32-rounded input")
    L.append("    std::array<double, kMaxSidebarLen> outL, outR;  // proto output, input-aligned")
    L.append("};")
    L.append("inline const SidebarFixture kSidebarFixtures[] = {")
    for name, xL, xR, outL, outR, maxCut, wetTrim, ms, link, focus, tA, tR, tD in sidebar:
        n = len(xL)
        xLp = list(xL) + [0.0] * (smaxlen - n)
        xRp = list(xR) + [0.0] * (smaxlen - n)
        oLp = list(outL) + [0.0] * (smaxlen - n)
        oRp = list(outR) + [0.0] * (smaxlen - n)
        L.append('    {"%s",' % name)
        L.append("     %.17g, %.17g, %s," % (maxCut, wetTrim, "true" if ms else "false"))
        L.append("     %.17g, %.17g," % (link, focus))
        L.append("     %.17g, %.17g, %.17g," % (tA, tR, tD))
        L.append("     5.0, 5.0, %.17g, %.17g, %d,"
                  % (DEFAULT_ATTACK_MS, DEFAULT_RELEASE_MS, n))
        L.append("     {{%s}}," % arr(xLp))
        L.append("     {{%s}}," % arr(xRp))
        L.append("     {{%s}}," % arr(oLp))
        L.append("     {{%s}}}," % arr(oRp))
    L.append("};")
    L.append("inline constexpr int kNumSidebarFixtures = %d;\n" % len(sidebar))

    L.append("}  // namespace specfix")

    out_path = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "test", "spectral_fixtures.h"))
    with open(out_path, "w") as fp:
        fp.write("\n".join(L) + "\n")
    print("Wrote %s: %d FFT fixtures, %d chain fixtures (max len %d), "
          "%d weight-chain fixtures (max len %d), %d sidebar fixtures (max len %d)"
          % (out_path, len(ffts), len(chains), maxlen, len(wchains), wmaxlen,
             len(sidebar), smaxlen))


if __name__ == "__main__":
    main()
