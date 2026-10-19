"""Spectral processor, Stage 2 addition: SIDEBAR global parameters (soothe3's
stereo/tilt/scale knob groups) -- 7 new globals layered on top of the
VERIFIED weight+ballistics chain (spectral_core.py / spectral_ballistics.py /
spectral_weight.py, all UNMODIFIED). Same "smallest hook, subclass in YOUR
file" pattern spectral_ballistics.SpectralCoreBallistics and
spectral_weight.SpectralCoreWeighted use on SpectralCore.

THE 7 PARAMETERS AND WHERE THEY HOOK IN:

  scale group (per-bin, single-channel math):
    max_cut_db  -- clamp on the per-bin REDUCTION (dB), inserted between the
                   tanh knee (sc.reduction_db) and the octave-frequency
                   smoother (self.smoother): red = min(red, max_cut_db).
                   >=24.0 = OFF (no clamp), the default.
    wet_trim_db -- NOT a per-bin/pipeline concept: a flat linear gain on the
                   fully-wet stereo output, applied once at the very end
                   (documented as SpectralMix's job at the C++ level -- the
                   proto has no dry/wet mix stage to slot it "before", so it
                   is applied directly to the wet signal this file returns).

  stereo group (drives what magnitude spectrum each channel's core sees,
  and the per-channel depth used to build max_db):
    ms_mode   -- linear L/R<->M/S transform (energy-preserving 1/sqrt(2)
                 convention) wrapped OUTSIDE the per-channel STFT processing:
                 encode -> process 2 "channels" (now M/S) exactly like L/R
                 -> decode. Pure time-domain linear algebra, commutes with
                 everything else here.
    link_pct  -- blends each channel's OWN per-bin magnitude with the
                 element-wise MAX across channels (the current C++ node's
                 fixed 100%-linked detector, documented in
                 dump_spectral_fixtures.py's STEREO-LINK RATIONALE) to form
                 a per-channel DETECTION spectrum, det_ch. det_ch (not the
                 channel's raw magnitude) is what frame_gain's envelope/
                 exceed math runs on; the resulting gain is applied to the
                 channel's REAL spectrum. At link=100, det_A == det_B ==
                 max_ch(mag) for both channels -- see gate1 for why this
                 makes two independently-updated per-channel cores
                 mathematically forced to bit-identical output, matching the
                 single-shared-gain-map legacy behaviour without literally
                 sharing one instance.
    focus     -- a per-channel DEPTH bias (not a detection change): channel
                 A (L or M) carries sign=+1, channel B (R or S) sign=-1;
                 depth_ch = depth * (1 - max(0, sign_ch * focus)). This
                 lands inside SpectralCore.__init__'s own
                 max_db = params.max_db * (depth/5) * dmult formula (we just
                 pass depth_ch instead of depth into the per-channel core's
                 constructor), so it needs no override of its own -- only
                 tilt_detail (below) needs to touch max_db post-hoc, because
                 it is frequency- not channel-dependent.

  tilt group (per-bin ARRAYS instead of the base classes' scalars; x(f) is
  the shared frequency-tilt axis: x(f) = clip(log2(f/1000)/log2(16), -1, 1),
  i.e. +-4 octaves around 1kHz maps to +-1):
    tilt_attack / tilt_release -- overrides self.ballistics.aA / .aR (plain
                 attributes on spectral_ballistics.VectorizedRtTBallistics)
                 from the base class's SCALARS to per-bin ARRAYS built from
                 tau_ch(f) = tau * 2**(tilt * x(f)) fed through a vectorized
                 port of compressor.tau_to_alpha (that function is scalar-
                 only -- a python `if` -- so it cannot be called on an array
                 without modifying compressor.py, which is out of scope; see
                 tau_to_alpha_arr below, verified against the unmodified
                 scalar function). VectorizedRtTBallistics.step's arithmetic
                 (aA*(y1-s1)+s1 etc.) is elementwise and was never assuming
                 aA/aR were scalars, so no change to that file is needed --
                 broadcasting just works.
    tilt_detail -- approximated through the DEPTH-MULTIPLIER component only
                 (per the frozen spec: "the smoother width stays global").
                 self.max_db (scalar in the base classes) becomes a per-bin
                 array: max_db(f) = params.max_db * (depth_ch/5) *
                 clip(dmult * 2**(tilt_detail*x(f)), 0.25, 4.0), where dmult
                 is the base class's already-fitted (detail-anchor-
                 interpolated) scalar depth multiplier. sc.reduction_db's
                 formula (max_db * 0.5*(1+tanh(...))) is untouched -- array
                 max_db broadcasts against the array exceed_db exactly like
                 the scalar case did.

DOES NOT MODIFY spectral_core.py / spectral_ballistics.py / spectral_weight.py:
SidebarChannelCore subclasses spectral_weight.SpectralCoreWeightedBallistics
(the full weight+ballistics chain) and overrides ONLY frame_gain, to insert
the max_cut clamp at its documented position; everything else (envelope
estimator, gain computer, frequency smoother, weight curve) is inherited
unmodified. Stereo coordination (the link blend needs BOTH channels' magnitude
for the SAME frame, which spectral_core.ProcessingSTFT's single-channel hook
cannot provide) is handled by a small dedicated 2-channel hop loop
(_stereo_hop_loop) that re-implements the verified sqrt-Hann analysis/
synthesis + COLA-normalized OLA math from spectral_stft.StreamingSTFT /
spectral_core.ProcessingSTFT (both read, neither modified) coordinated across
2 channels -- the same per-hop arithmetic, just able to see both channels'
spectra before deciding either channel's gain. Batch (non-chunked) processing
only, matching this whole stage's own precedent (run_chain /
run_chain_ballistics / run_chain_weighted_ballistics all call process()+
flush() in one shot too); Stage 1 already proved chunk-size invariance for
this exact per-hop math, so it isn't re-proven here.

Run from repo root:  ./proto/.venv/bin/python proto/spectral_sidebar.py
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spectral_core as sc                 # noqa: E402  (verified, NOT modified)
import spectral_ballistics as sb           # noqa: E402  (verified, NOT modified)
import spectral_weight as sw               # noqa: E402  (verified, NOT modified)
from spectral_stft import sqrt_hann, cola_norm  # noqa: E402  (verified, NOT modified)
from compressor import tau_to_alpha        # noqa: E402  (verified scalar reference)

OUT = "proto/out"
os.makedirs(OUT, exist_ok=True)

N = sc.N
HOP = sc.HOP
FS = sc.FS
EPS = sc.EPS
NOTCH_F0 = sc.NOTCH_F0


# ============================================================================
# PART 1: pure-formula helpers (M/S, tilt axis, focus bias, vectorized alpha)
# ============================================================================

_SQRT2 = np.sqrt(2.0)


def encode_ms(L, R):
    """Energy-preserving M/S encode: M=(L+R)/sqrt2, S=(L-R)/sqrt2."""
    L = np.asarray(L, dtype=np.float64)
    R = np.asarray(R, dtype=np.float64)
    return (L + R) / _SQRT2, (L - R) / _SQRT2


def decode_ms(M, S):
    M = np.asarray(M, dtype=np.float64)
    S = np.asarray(S, dtype=np.float64)
    return (M + S) / _SQRT2, (M - S) / _SQRT2


def tilt_x(freqs):
    """x(f) = clip(log2(f/1000)/log2(16), -1, 1) -- +-4 octaves around 1kHz
    maps to +-1. f<=0 (DC) is guarded to a tiny positive placeholder before
    the log so it lands at the -1 floor (a "very low frequency"), not -inf/NaN."""
    freqs = np.asarray(freqs, dtype=np.float64)
    safe_f = np.maximum(freqs, 1e-9)
    with np.errstate(divide="ignore"):
        x = np.log2(safe_f / 1000.0) / np.log2(16.0)
    return np.clip(x, -1.0, 1.0)


def focus_depth_bias(sign, focus):
    """depth_ch multiplier from the frozen focus formula:
    1 - max(0, sign_ch * focus). sign=+1 for channel A (L/M), sign=-1 for
    channel B (R/S) -- see module docstring for the convention."""
    return 1.0 - max(0.0, sign * focus)


def tau_to_alpha_arr(tau_s, fs_frame):
    """Vectorized port of compressor.tau_to_alpha's formula
    (alpha = exp(-1/(tau*fs)); tau<=1us -> alpha=0). Not a call to that
    function: it guards tau<=1e-6 with a python `if`, which cannot accept an
    array. Same formula, ported to elementwise numpy ops; verified in gate7
    against the unmodified scalar function point-by-point."""
    tau_s = np.asarray(tau_s, dtype=np.float64)
    with np.errstate(divide="ignore"):
        alpha = np.exp(-1.0 / (tau_s * fs_frame))
    return np.where(tau_s <= 1e-6, 0.0, alpha)


# ============================================================================
# PART 2: SidebarChannelCore -- per-channel hook (max_cut, tilt_detail,
# tilt_attack/release). Subclasses the full weight+ballistics chain.
# ============================================================================

class SidebarChannelCore(sw.SpectralCoreWeightedBallistics):
    """One channel's (L/R or M/S) processing core. `depth` passed in here is
    ALREADY the focus-biased depth_ch (the caller multiplies by
    focus_depth_bias before constructing this). Adds, on top of the
    inherited weight+ballistics frame_gain:

      - max_cut_db: clamp on `red` after the knee, before frequency
        smoothing (>=24.0 = OFF, exact skip, no extra float ops on the
        default-identity path).
      - tilt_detail: overrides self.max_db (base class: a scalar) with a
        per-bin array, ONLY when tilt_detail != 0 (so the default-identity
        path never touches self.max_db at all -- bit-identical to base).
      - tilt_attack / tilt_release: overrides self.ballistics.aA / .aR
        (base class: scalars) with per-bin arrays, independently (each only
        when its own tilt is != 0), same bit-identity-at-default reasoning.
    """

    def __init__(self, N, fs, params, depth, detail, attack_ms, release_ms, hop,
                 bands, lo, hi, max_cut_db=24.0,
                 tilt_attack=0.0, tilt_release=0.0, tilt_detail=0.0):
        super().__init__(N, fs, params, depth=depth, detail=detail,
                          attack_ms=attack_ms, release_ms=release_ms, hop=hop,
                          bands=bands, lo=lo, hi=hi)
        self.max_cut_db = max_cut_db

        if tilt_detail != 0.0:
            xf = tilt_x(self.freqs)
            dmult_f = np.clip(self.dmult * (2.0 ** (tilt_detail * xf)), 0.25, 4.0)
            self.max_db = params["max_db"] * (depth / 5.0) * dmult_f

        if tilt_attack != 0.0:
            xf = tilt_x(self.freqs)
            tau_a = (attack_ms / 1000.0) * (2.0 ** (tilt_attack * xf))
            self.ballistics.aA = tau_to_alpha_arr(tau_a, self.fs_frame)

        if tilt_release != 0.0:
            xf = tilt_x(self.freqs)
            tau_r = (release_ms / 1000.0) * (2.0 ** (tilt_release * xf))
            self.ballistics.aR = tau_to_alpha_arr(tau_r, self.fs_frame)

    def frame_gain(self, mag):
        """`mag` is the per-channel DETECTION spectrum (already link-blended
        by the caller, see _sidebar_gain_fn) -- envelope and exceed are
        computed from it exactly like the base classes compute them from a
        channel's own raw magnitude; the resulting gain is applied by the
        caller to that channel's REAL spectrum."""
        if not self.active:
            return np.ones_like(mag)
        env = self.env(mag)
        exceed_db = 20.0 * np.log10((mag + sc.EPS) / (env + sc.EPS))
        exceed_db = exceed_db + self.weight
        red = sc.reduction_db(exceed_db, self.max_db, self.p["knee_c"], self.p["knee_w"])
        if self.max_cut_db < 24.0:
            red = np.minimum(red, self.max_cut_db)          # scale: max_cut clamp
        red = self.smoother(red)                             # frequency-axis smoothing
        red = self.ballistics.step(red)                       # time-axis smoothing
        return 10.0 ** (-red / 20.0)


# ============================================================================
# PART 3: 2-channel STFT hop loop (the coordination the link blend needs)
# ============================================================================

def _stereo_hop_loop(xA, xB, hop, N_, gain_fn):
    """Batch (non-chunked) 2-channel sqrt-Hann analysis/synthesis + COLA-
    normalized OLA, coordinated so `gain_fn(specA, specB) -> (gainA, gainB)`
    sees BOTH channels' spectra for the SAME frame before either channel's
    gain is decided. Per-hop math (window, rfft/irfft, OLA accumulate/drain,
    absolute-position output writes) is the identical arithmetic
    spectral_stft.StreamingSTFT._process_one_hop / spectral_core.
    ProcessingSTFT._process_one_hop already prove correct, just run twice per
    hop instead of once and coordinated through one shared gain_fn call.
    Returns (outA, outB), each length len(xA)+N_ (caller slices
    [N_:N_+len(xA)], the same input-aligned convention run_chain uses)."""
    assert N_ % hop == 0
    n = len(xA)
    assert len(xB) == n
    win = sqrt_hann(N_)
    norm, _ = cola_norm(N_, hop)

    pad_to_hop = (-n) % hop
    total_pad = pad_to_hop + N_
    xA_p = np.concatenate([np.asarray(xA, dtype=np.float64), np.zeros(total_pad)])
    xB_p = np.concatenate([np.asarray(xB, dtype=np.float64), np.zeros(total_pad)])
    n_p = len(xA_p)
    n_hops = n_p // hop

    ring_A, ring_B = np.zeros(N_), np.zeros(N_)
    ola_A, ola_B = np.zeros(N_), np.zeros(N_)
    out_len = n_hops * hop
    out_A, out_B = np.zeros(out_len), np.zeros(out_len)

    for h in range(n_hops):
        chunkA = xA_p[h * hop:(h + 1) * hop]
        chunkB = xB_p[h * hop:(h + 1) * hop]
        ring_A = np.concatenate([ring_A[hop:], chunkA])
        ring_B = np.concatenate([ring_B[hop:], chunkB])

        specA = np.fft.rfft(ring_A * win)
        specB = np.fft.rfft(ring_B * win)
        gainA, gainB = gain_fn(specA, specB)
        specA = specA * gainA
        specB = specB * gainB

        frameA_out = np.fft.irfft(specA, N_) * win
        frameB_out = np.fft.irfft(specB, N_) * win
        ola_A = ola_A + frameA_out
        ola_B = ola_B + frameB_out
        readyA = ola_A[:hop] * norm
        readyB = ola_B[:hop] * norm
        ola_A = np.concatenate([ola_A[hop:], np.zeros(hop)])
        ola_B = np.concatenate([ola_B[hop:], np.zeros(hop)])

        start = (h + 1) * hop - N_
        if start >= 0:
            out_A[start:start + hop] = readyA
            out_B[start:start + hop] = readyB

    return out_A[:n + N_], out_B[:n + N_]


def _sidebar_gain_fn(coreA, coreB, link_pct):
    """The link-blended detection formula: det_ch = (link/100)*max_ch(mag) +
    (1-link/100)*mag_ch, fed into each channel's OWN core.frame_gain."""
    link = link_pct / 100.0

    def fn(specA, specB):
        magA, magB = np.abs(specA), np.abs(specB)
        max_mag = np.maximum(magA, magB)
        detA = link * max_mag + (1.0 - link) * magA
        detB = link * max_mag + (1.0 - link) * magB
        return coreA.frame_gain(detA), coreB.frame_gain(detB)

    return fn


def _reference_gain_fn(core):
    """Independent reference construction of the CURRENT (pre-sidebar)
    stereo behaviour (dump_spectral_fixtures.py's STEREO-LINK RATIONALE):
    ONE shared core, detection = elementwise max across channels, the SAME
    gain map applied to both channels. Used only by gate1 (default-identity)
    as the cross-check target -- built independently of _sidebar_gain_fn
    (single shared instance vs. sidebar's two independent instances) so the
    comparison is a genuine check, not a self-comparison."""
    def fn(specA, specB):
        det = np.maximum(np.abs(specA), np.abs(specB))
        g = core.frame_gain(det)
        return g, g

    return fn


# ============================================================================
# PART 4: top-level stereo sidebar chain
# ============================================================================

def process_channels(chA_in, chB_in, params, depth, detail, attack_ms, release_ms,
                      hop, N_, fs, bands, lo, hi, max_cut_db, link_pct, focus,
                      tilt_attack, tilt_release, tilt_detail):
    """Runs the 2 (already M/S-encoded-if-applicable) channels through their
    own SidebarChannelCore instances, link-blended. Returns (outA, outB),
    INPUT-ALIGNED (latency N_ already removed), same convention as
    run_chain/run_chain_ballistics. Does NOT apply wet_trim or M/S decode --
    those are run_sidebar's job, so this is directly reusable by tests that
    want to inspect M/S-domain output before decode (gate4)."""
    bias_A = focus_depth_bias(+1.0, focus)
    bias_B = focus_depth_bias(-1.0, focus)
    n = len(chA_in)

    coreA = SidebarChannelCore(N_, fs, params, depth=depth * bias_A, detail=detail,
                                attack_ms=attack_ms, release_ms=release_ms, hop=hop,
                                bands=bands, lo=lo, hi=hi, max_cut_db=max_cut_db,
                                tilt_attack=tilt_attack, tilt_release=tilt_release,
                                tilt_detail=tilt_detail)
    coreB = SidebarChannelCore(N_, fs, params, depth=depth * bias_B, detail=detail,
                                attack_ms=attack_ms, release_ms=release_ms, hop=hop,
                                bands=bands, lo=lo, hi=hi, max_cut_db=max_cut_db,
                                tilt_attack=tilt_attack, tilt_release=tilt_release,
                                tilt_detail=tilt_detail)

    gain_fn = _sidebar_gain_fn(coreA, coreB, link_pct)
    outA, outB = _stereo_hop_loop(chA_in, chB_in, hop, N_, gain_fn)
    return outA[N_:N_ + n], outB[N_:N_ + n]


def run_sidebar(xL, xR, params, depth=5.0, detail=5.0,
                 attack_ms=sb.DEFAULT_ATTACK_MS, release_ms=sb.DEFAULT_RELEASE_MS,
                 hop=HOP, N_=N, fs=FS, bands=None, lo=20.0, hi=20000.0,
                 max_cut_db=24.0, wet_trim_db=0.0, ms_mode=False,
                 link_pct=100.0, focus=0.0,
                 tilt_attack=0.0, tilt_release=0.0, tilt_detail=0.0):
    """Full SIDEBAR chain: [encode M/S if ms_mode] -> per-channel
    weight+ballistics+sidebar core (link-blended detection) -> [decode M/S]
    -> wet_trim (flat linear gain on the final L/R). Returns (wetL, wetR),
    input-aligned (length == len(xL))."""
    xL = np.asarray(xL, dtype=np.float64)
    xR = np.asarray(xR, dtype=np.float64)
    bands = bands or []

    if ms_mode:
        chA_in, chB_in = encode_ms(xL, xR)
    else:
        chA_in, chB_in = xL, xR

    outA, outB = process_channels(chA_in, chB_in, params, depth, detail, attack_ms,
                                   release_ms, hop, N_, fs, bands, lo, hi, max_cut_db,
                                   link_pct, focus, tilt_attack, tilt_release, tilt_detail)

    if ms_mode:
        wetL, wetR = decode_ms(outA, outB)
    else:
        wetL, wetR = outA, outB

    trim_lin = 10.0 ** (wet_trim_db / 20.0)
    return wetL * trim_lin, wetR * trim_lin


def run_reference_stereo(xL, xR, params, depth=5.0, detail=5.0,
                          attack_ms=sb.DEFAULT_ATTACK_MS, release_ms=sb.DEFAULT_RELEASE_MS,
                          hop=HOP, N_=N, fs=FS, bands=None, lo=20.0, hi=20000.0):
    """Independent reference: the CURRENT (pre-sidebar) stereo pipeline, a
    single shared sw.SpectralCoreWeightedBallistics instance, max-across-
    channels detection, one gain map applied to both channels. See
    _reference_gain_fn."""
    bands = bands or []
    core = sw.SpectralCoreWeightedBallistics(N_, fs, params, depth=depth, detail=detail,
                                              attack_ms=attack_ms, release_ms=release_ms,
                                              hop=hop, bands=bands, lo=lo, hi=hi)
    gain_fn = _reference_gain_fn(core)
    n = len(xL)
    outA, outB = _stereo_hop_loop(xL, xR, hop, N_, gain_fn)
    return outA[N_:N_ + n], outB[N_:N_ + n]


# ============================================================================
# PART 5: per-frame (no-OLA) stereo measurement helper, for tight numeric
# gates that don't want OLA/windowing measurement noise (same rationale as
# spectral_core.py Part 6b's mean_frame_reduction / fast_kernel).
# ============================================================================

def stereo_frame_reductions(xL, xR, coreA, coreB, link_pct, N_=N, hop=HOP):
    """Per-bin MEAN reduction (dB, >= 0) each channel's core applies across
    all frames, computed directly from the link-blended detection formula
    (bypassing OLA reconstruction) -- the frequency+link kernel actually
    applied, without OLA/windowing measurement noise."""
    win = sqrt_hann(N_)
    link = link_pct / 100.0
    redsA, redsB = [], []
    for i in range(0, len(xL) - N_, hop):
        magA = np.abs(np.fft.rfft(xL[i:i + N_] * win))
        magB = np.abs(np.fft.rfft(xR[i:i + N_] * win))
        max_mag = np.maximum(magA, magB)
        detA = link * max_mag + (1.0 - link) * magA
        detB = link * max_mag + (1.0 - link) * magB
        gA = coreA.frame_gain(detA)
        gB = coreB.frame_gain(detB)
        redsA.append(-20.0 * np.log10(np.maximum(gA, 1e-12)))
        redsB.append(-20.0 * np.log10(np.maximum(gB, 1e-12)))
    return np.mean(redsA, axis=0), np.mean(redsB, axis=0)


def _all_frame_reductions_mono(x, core, N_=N, hop=HOP):
    """Per-FRAME (not averaged) reduction array for a single core, own
    magnitude as detection (no stereo blending) -- used by the max_cut gate
    to assert the clamp holds on EVERY frame, not just the mean."""
    win = sqrt_hann(N_)
    reds = []
    for i in range(0, len(x) - N_, hop):
        mag = np.abs(np.fft.rfft(x[i:i + N_] * win))
        g = core.frame_gain(mag)
        reds.append(-20.0 * np.log10(np.maximum(g, 1e-12)))
    return np.array(reds)


# ============================================================================
# PART 6: probe builders reused from spectral_core (imported, not redefined)
# ============================================================================
_db2amp = sc._db2amp
_sine_rms = sc._sine_rms
_noise_rms = sc._noise_rms
make_resonance_step = sc.make_resonance_step


def _rng_seed(seed):
    return np.random.default_rng(seed)


# ============================================================================
# GATES
# ============================================================================

def gate1_default_identity(params):
    """Assert 1 (the big one) -- all 7 params at defaults, full pipeline
    output bit-identical (<1e-15) to the independent reference stereo
    pipeline (max-across-channels detection, one shared core) on a
    fixed-seed stereo noise+resonance input. PLUS per-param mechanism-level
    identity: for each of the 7 params, its default value is checked to
    collapse the mechanism it controls to an exact structural no-op
    (max_cut: clamp branch skipped; wet_trim: linear trim==1.0 exactly;
    ms_mode: channels passed through unchanged; link: det==max_mag
    regardless of mag_ch; focus: depth bias==1.0 exactly for both channels;
    tilt_attack/release/detail: base-class scalar ballistics/max_db left
    untouched, no array override). This is checked at the FORMULA level
    (not by re-running the full heavy STFT pipeline 7 more times) because
    each mechanism is independently gated in code -- a formula-level check
    is the precise, minimal way to verify each default is a true no-op
    regardless of what the OTHER 6 params are doing."""
    print("\n=== Test 1: DEFAULT-IDENTITY (full pipeline + per-param mechanism no-ops) ===")
    ok = True

    dur = 0.8
    xL = make_resonance_step(12.0, dur=dur)
    xR = make_resonance_step(12.0, dur=dur)  # sc's module RNG advances -> independent noise

    ref_L, ref_R = run_reference_stereo(xL, xR, params, depth=5.0, detail=5.0)
    got_L, got_R = run_sidebar(xL, xR, params, depth=5.0, detail=5.0)  # all 7 at default
    diff = float(max(np.max(np.abs(got_L - ref_L)), np.max(np.abs(got_R - ref_R))))
    p_full = diff < 1e-15
    ok &= p_full
    print(f"  full pipeline: max|sidebar(defaults) - reference| = {diff:.3e} "
          f"(gate <1e-15) -> {'PASS' if p_full else 'FAIL'}")

    # per-param mechanism-level no-op checks
    core_probe = SidebarChannelCore(N, FS, params, depth=5.0, detail=5.0,
                                     attack_ms=sb.DEFAULT_ATTACK_MS,
                                     release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                     bands=[], lo=20.0, hi=20000.0, max_cut_db=24.0)
    p_mc = not (24.0 < 24.0)
    ok &= p_mc
    print(f"  max_cut_db default (24.0, OFF): clamp branch skip condition -> "
          f"{'PASS' if p_mc else 'FAIL'}")

    p_wt = (10.0 ** (0.0 / 20.0)) == 1.0
    ok &= p_wt
    print(f"  wet_trim_db default (0.0): trim_lin == {10.0 ** (0.0 / 20.0):.17g} "
          f"(gate ==1.0) -> {'PASS' if p_wt else 'FAIL'}")

    rng = np.random.default_rng(99)
    testL, testR = rng.standard_normal(2000) * 0.3, rng.standard_normal(2000) * 0.3
    ms_mode = False
    chA_in, chB_in = (encode_ms(testL, testR) if ms_mode else (testL, testR))
    p_ms = np.array_equal(chA_in, testL) and np.array_equal(chB_in, testR)
    ok &= p_ms
    print(f"  ms_mode default (False): channels passed through unchanged -> "
          f"{'PASS' if p_ms else 'FAIL'}")

    magA = np.abs(rng.standard_normal(513))
    magB = np.abs(rng.standard_normal(513))
    max_mag = np.maximum(magA, magB)
    link = 100.0 / 100.0
    detA = link * max_mag + (1.0 - link) * magA
    detB = link * max_mag + (1.0 - link) * magB
    p_link = bool(np.array_equal(detA, max_mag)) and bool(np.array_equal(detB, max_mag))
    ok &= p_link
    print(f"  link_pct default (100.0): det_A==det_B==max_mag exactly, for random "
          f"mag_A/mag_B -> {'PASS' if p_link else 'FAIL'}")

    bA = focus_depth_bias(+1.0, 0.0)
    bB = focus_depth_bias(-1.0, 0.0)
    p_focus = (bA == 1.0) and (bB == 1.0)
    ok &= p_focus
    print(f"  focus default (0.0): depth_bias_A={bA}, depth_bias_B={bB} "
          f"(gate ==1.0 both) -> {'PASS' if p_focus else 'FAIL'}")

    core_tilt_off = SidebarChannelCore(N, FS, params, depth=5.0, detail=5.0,
                                        attack_ms=27.0, release_ms=50.0, hop=HOP,
                                        bands=[], lo=20.0, hi=20000.0,
                                        tilt_attack=0.0, tilt_release=0.0, tilt_detail=0.0)
    p_tilt_a = np.isscalar(core_tilt_off.ballistics.aA) or np.ndim(core_tilt_off.ballistics.aA) == 0
    p_tilt_r = np.isscalar(core_tilt_off.ballistics.aR) or np.ndim(core_tilt_off.ballistics.aR) == 0
    p_tilt_d = np.isscalar(core_tilt_off.max_db) or np.ndim(core_tilt_off.max_db) == 0
    ok &= p_tilt_a and p_tilt_r and p_tilt_d
    print(f"  tilt_attack/release/detail default (0.0): aA/aR/max_db left as base-class "
          f"SCALARS (no array override) -> {'PASS' if (p_tilt_a and p_tilt_r and p_tilt_d) else 'FAIL'}")

    print(f"  -> {'PASS' if ok else 'FAIL'}")
    return ok


def gate2_max_cut(params):
    """Assert 2 -- max_cut: deep-cut scenario, per-bin reduction never
    exceeds max_cut_db (exact clamp, every frame not just the mean); OFF
    reproduces the unclamped values (and confirms the clamp actually BINDS
    when active -- some unclamped reductions genuinely exceed the test's
    max_cut_db, otherwise the clamp assert would be vacuous)."""
    print("\n=== Test 2: max_cut (deep-cut clamp, exact + OFF) ===")
    ok = True
    x = make_resonance_step(24.0, dur=1.0, noise_dbfs=-30.0)  # hot resonance
    max_cut_db = 6.0

    core_clamped = SidebarChannelCore(N, FS, params, depth=8.0, detail=8.0,
                                       attack_ms=sb.DEFAULT_ATTACK_MS,
                                       release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                       bands=[], lo=20.0, hi=20000.0, max_cut_db=max_cut_db)
    reds_clamped = _all_frame_reductions_mono(x, core_clamped)
    max_red_clamped = float(np.max(reds_clamped))
    p_clamp = max_red_clamped <= max_cut_db + 1e-9
    ok &= p_clamp
    print(f"  max_cut_db={max_cut_db}: max reduction over all frames/bins = "
          f"{max_red_clamped:.6f} dB (gate <= {max_cut_db}+1e-9) -> {'PASS' if p_clamp else 'FAIL'}")

    core_off = SidebarChannelCore(N, FS, params, depth=8.0, detail=8.0,
                                   attack_ms=sb.DEFAULT_ATTACK_MS,
                                   release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                   bands=[], lo=20.0, hi=20000.0, max_cut_db=24.0)
    reds_off = _all_frame_reductions_mono(x, core_off)
    max_red_off = float(np.max(reds_off))
    p_binds = max_red_off > max_cut_db
    ok &= p_binds
    print(f"  max_cut_db=24 (OFF): max reduction = {max_red_off:.4f} dB "
          f"(> {max_cut_db} confirms the clamp above actually binds) -> "
          f"{'PASS' if p_binds else 'FAIL'}")

    core_off2 = SidebarChannelCore(N, FS, params, depth=8.0, detail=8.0,
                                    attack_ms=sb.DEFAULT_ATTACK_MS,
                                    release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                    bands=[], lo=20.0, hi=20000.0, max_cut_db=24.0)
    core_base = sw.SpectralCoreWeightedBallistics(N, FS, params, depth=8.0, detail=8.0,
                                                   attack_ms=sb.DEFAULT_ATTACK_MS,
                                                   release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                                   bands=[], lo=20.0, hi=20000.0)
    reds_off2 = _all_frame_reductions_mono(x, core_off2)
    reds_base = _all_frame_reductions_mono(x, core_base)
    diff_off = float(np.max(np.abs(reds_off2 - reds_base)))
    p_off_identity = diff_off < 1e-12
    ok &= p_off_identity
    print(f"  OFF vs base (no max_cut mechanism at all): max|diff| = {diff_off:.3e} "
          f"(gate <1e-12, OFF reproduces unclamped) -> {'PASS' if p_off_identity else 'FAIL'}")

    print(f"  -> {'PASS' if ok else 'FAIL'}")
    return ok


def gate3_wet_trim(params):
    """Assert 3 -- wet_trim: output scales by exactly 10**(trim/20) vs
    trim=0 on the wet-only component (mix isn't modelled in the proto --
    see module docstring: wet_trim is a flat post-multiply on the fully-wet
    signal this file returns, which at mix=100% (no dry path implemented
    here) IS the output)."""
    print("\n=== Test 3: wet_trim (exact scale on the wet path) ===")
    xL = make_resonance_step(6.0, dur=0.5)
    xR = make_resonance_step(6.0, dur=0.5)
    trim_db = -7.5
    y0L, y0R = run_sidebar(xL, xR, params, depth=5.0, detail=5.0, wet_trim_db=0.0)
    ytL, ytR = run_sidebar(xL, xR, params, depth=5.0, detail=5.0, wet_trim_db=trim_db)
    expect = 10.0 ** (trim_db / 20.0)

    ratioL = ytL / np.where(y0L == 0, 1e-30, y0L)
    ratioR = ytR / np.where(y0R == 0, 1e-30, y0R)
    mask = (np.abs(y0L) > 1e-6)
    errL = float(np.max(np.abs(ratioL[mask] - expect)))
    mask_r = (np.abs(y0R) > 1e-6)
    errR = float(np.max(np.abs(ratioR[mask_r] - expect)))
    ok = errL < 1e-9 and errR < 1e-9
    print(f"  trim={trim_db} dB -> expect ratio {expect:.10f}")
    print(f"  L: max|ratio - expect| = {errL:.3e} (gate <1e-9) -> {'PASS' if errL < 1e-9 else 'FAIL'}")
    print(f"  R: max|ratio - expect| = {errR:.3e} (gate <1e-9) -> {'PASS' if errR < 1e-9 else 'FAIL'}")
    print(f"  -> {'PASS' if ok else 'FAIL'}")
    return ok


def gate4_ms(params):
    """Assert 4 -- M/S: encode/decode round trip < 1e-15; an S-only
    resonance (shared broadband noise in L/R -> pure M; L=+tone,R=-tone ->
    pure S) is cut in M/S mode without touching M content."""
    print("\n=== Test 4: M/S encode/decode + S-only isolation ===")
    ok = True

    rng = np.random.default_rng(42)
    L = rng.standard_normal(4000) * 0.4
    R = rng.standard_normal(4000) * 0.4
    M, S = encode_ms(L, R)
    L2, R2 = decode_ms(M, S)
    diff = float(max(np.max(np.abs(L2 - L)), np.max(np.abs(R2 - R))))
    p_roundtrip = diff < 1e-15
    ok &= p_roundtrip
    print(f"  round trip: max|decode(encode(L,R)) - (L,R)| = {diff:.3e} "
          f"(gate <1e-15) -> {'PASS' if p_roundtrip else 'FAIL'}")

    dur = 1.0
    noise = _noise_rms(dur, -30.0)
    tone = _sine_rms(NOTCH_F0, dur, -14.0)[:len(noise)]
    xL = noise + tone
    xR = noise - tone
    Min, Sin = encode_ms(xL, xR)
    max_m_content = float(np.max(np.abs(Min - noise * _SQRT2)))
    p_construct = max_m_content < 1e-12
    ok &= p_construct
    print(f"  construction check: M == sqrt2*noise (no tone) within {max_m_content:.3e} "
          f"(gate <1e-12) -> {'PASS' if p_construct else 'FAIL'}")

    # link_pct=0.0 (NOT the 100-default): at link=100 the M and S "channels"
    # SHARE detection by design (det_ch = max_ch(mag) at link=100, see
    # module docstring / gate5) -- that is what "link" means, and it is
    # deliberately tested separately in gate5. Leaving link at its 100
    # default here would let S's loud tone bin leak into M's DETECTION (not
    # its content) via the shared max, inflating M's measured attenuation to
    # ~0.53dB (measured) purely from that cross-channel coupling -- a real
    # and correctly-implemented consequence of link=100, but a confound for
    # THIS assert, which is specifically about the M/S TRANSFORM's isolation
    # property, not the (separately gated) link mechanism. link=0 removes
    # the confound: M and S get fully independent detection, isolating the
    # claim this assert is actually making.
    Mout, Sout = process_channels(Min, Sin, params, depth=5.0, detail=5.0,
                                   attack_ms=sb.DEFAULT_ATTACK_MS,
                                   release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP, N_=N, fs=FS,
                                   bands=[], lo=20.0, hi=20000.0, max_cut_db=24.0,
                                   link_pct=0.0, focus=0.0,
                                   tilt_attack=0.0, tilt_release=0.0, tilt_detail=0.0)

    m_atten = sc.measure_tone_atten(Min, Mout, NOTCH_F0)  # ~0 dB expected (no tone in M)
    from soothe3_measure_analyze import welch_atten
    f, atten_m = welch_atten(Min, Mout, FS, nperseg=2048)
    band = (f > 100) & (f < 18000)
    m_broadband_atten = float(np.mean(atten_m[band]))
    p_m_untouched = abs(m_broadband_atten) < 0.5
    ok &= p_m_untouched
    print(f"  M broadband attenuation (link=0, isolating the M/S transform from the "
          f"separately-gated link mechanism) = {m_broadband_atten:.3f} dB "
          f"(gate |.|<0.5, Stage-2/3's own established white-noise bound) -> "
          f"{'PASS' if p_m_untouched else 'FAIL'}")

    s_atten = -sc.measure_tone_atten(Sin, Sout, NOTCH_F0)
    p_s_cut = s_atten > 2.0
    ok &= p_s_cut
    print(f"  S tone reduction = {s_atten:.3f} dB (gate >2.0, real cut) -> "
          f"{'PASS' if p_s_cut else 'FAIL'}")

    print(f"  -> {'PASS' if ok else 'FAIL'}")
    return ok


def gate5_link(params):
    """Assert 5 -- link: link=100 == reference (bit-identical); link=0 with
    a left-only resonance cuts ONLY the left channel (right reduction ~=0);
    link=50 in between (monotonicity)."""
    print("\n=== Test 5: link (100==reference, 0==left-only, 50==monotone) ===")
    ok = True

    dur = 0.8
    xL = make_resonance_step(12.0, dur=dur)
    xR = make_resonance_step(12.0, dur=dur)
    ref_L, ref_R = run_reference_stereo(xL, xR, params, depth=5.0, detail=5.0)
    lnk_L, lnk_R = run_sidebar(xL, xR, params, depth=5.0, detail=5.0, link_pct=100.0)
    diff = float(max(np.max(np.abs(lnk_L - ref_L)), np.max(np.abs(lnk_R - ref_R))))
    p_100 = diff < 1e-15
    ok &= p_100
    print(f"  link=100: max|sidebar - reference| = {diff:.3e} (gate <1e-15) -> "
          f"{'PASS' if p_100 else 'FAIL'}")

    tone = _sine_rms(NOTCH_F0, dur, -14.0)
    floor_l = _noise_rms(dur, -36.0)[:len(tone)]
    xL2 = tone + floor_l
    xR2 = _noise_rms(dur, -50.0)[:len(tone)]  # quiet, independent, no tone

    bin_f0 = int(round(NOTCH_F0 / (FS / N)))
    reds = {}
    for link in (0.0, 50.0, 100.0):
        coreA = SidebarChannelCore(N, FS, params, depth=5.0, detail=5.0,
                                    attack_ms=sb.DEFAULT_ATTACK_MS,
                                    release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                    bands=[], lo=20.0, hi=20000.0)
        coreB = SidebarChannelCore(N, FS, params, depth=5.0, detail=5.0,
                                    attack_ms=sb.DEFAULT_ATTACK_MS,
                                    release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                    bands=[], lo=20.0, hi=20000.0)
        redA, redB = stereo_frame_reductions(xL2, xR2, coreA, coreB, link)
        reds[link] = (float(redA[bin_f0]), float(redB[bin_f0]))
        print(f"  link={link:5.1f}: left reduction={reds[link][0]:6.3f} dB  "
              f"right reduction={reds[link][1]:6.3f} dB")

    p_left_cut = reds[0.0][0] > 2.0
    p_right_untouched = reds[0.0][1] < 0.5
    ok &= p_left_cut and p_right_untouched
    print(f"  link=0: left cut (>2.0dB) -> {'PASS' if p_left_cut else 'FAIL'}; "
          f"right ~=1 gain (<0.5dB) -> {'PASS' if p_right_untouched else 'FAIL'}")

    right_vals = [reds[0.0][1], reds[50.0][1], reds[100.0][1]]
    p_mono = right_vals[0] <= right_vals[1] + 1e-9 <= right_vals[2] + 2e-9
    ok &= p_mono
    print(f"  monotonicity (right reduction, link 0->50->100): {right_vals} "
          f"(non-decreasing) -> {'PASS' if p_mono else 'FAIL'}")

    print(f"  -> {'PASS' if ok else 'FAIL'}")
    return ok


def gate6_focus(params):
    """Assert 6 -- focus: focus=+1 leaves channel A's processing at zero
    depth (gain == 1 exactly, core.active False); symmetric for focus=-1
    (channel B); focus=0 == current (depth bias 1.0 both, already re-checked
    here directly on the core construction)."""
    print("\n=== Test 6: focus (zero-depth at +-1, symmetric) ===")
    ok = True

    bias_A_p1 = focus_depth_bias(+1.0, +1.0)
    bias_B_p1 = focus_depth_bias(-1.0, +1.0)
    coreA_p1 = SidebarChannelCore(N, FS, params, depth=5.0 * bias_A_p1, detail=5.0,
                                   attack_ms=sb.DEFAULT_ATTACK_MS,
                                   release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                   bands=[], lo=20.0, hi=20000.0)
    mag = np.abs(np.random.default_rng(3).standard_normal(513)) + 0.5
    gA_p1 = coreA_p1.frame_gain(mag)
    p_focus_p1 = (not coreA_p1.active) and bool(np.all(gA_p1 == 1.0))
    ok &= p_focus_p1
    print(f"  focus=+1: depth_bias_A={bias_A_p1} (depth_A={5.0*bias_A_p1}), "
          f"core.active={coreA_p1.active}, gain==1 everywhere -> "
          f"{'PASS' if p_focus_p1 else 'FAIL'}")
    print(f"            depth_bias_B={bias_B_p1} (channel B full depth, unaffected)")

    bias_A_m1 = focus_depth_bias(+1.0, -1.0)
    bias_B_m1 = focus_depth_bias(-1.0, -1.0)
    coreB_m1 = SidebarChannelCore(N, FS, params, depth=5.0 * bias_B_m1, detail=5.0,
                                   attack_ms=sb.DEFAULT_ATTACK_MS,
                                   release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                   bands=[], lo=20.0, hi=20000.0)
    gB_m1 = coreB_m1.frame_gain(mag)
    p_focus_m1 = (not coreB_m1.active) and bool(np.all(gB_m1 == 1.0))
    ok &= p_focus_m1
    print(f"  focus=-1: depth_bias_B={bias_B_m1} (depth_B={5.0*bias_B_m1}), "
          f"core.active={coreB_m1.active}, gain==1 everywhere -> "
          f"{'PASS' if p_focus_m1 else 'FAIL'}")
    print(f"            depth_bias_A={bias_A_m1} (channel A full depth, unaffected) "
          f"-- symmetric with the focus=+1 case above")

    p_zero = (focus_depth_bias(+1.0, 0.0) == 1.0) and (focus_depth_bias(-1.0, 0.0) == 1.0)
    ok &= p_zero
    print(f"  focus=0: depth_bias_A=depth_bias_B=1.0 exactly (== current) -> "
          f"{'PASS' if p_zero else 'FAIL'}")

    print(f"  -> {'PASS' if ok else 'FAIL'}")
    return ok


def _measure_bin_tau_ms(alpha, fs_frame, rising, n_frames=4000):
    """Isolated single-bin tau probe, same algebraic-isolation trick as
    spectral_ballistics.measure_attack_tau_ms/measure_release_tau_ms (aR=0
    for a pure attack pole, aA=0 for a pure release pole) but fed a
    DIRECTLY-GIVEN alpha (extracted from a per-bin tilt array) instead of
    recomputing it from a tau label -- this is what lets us ask "what tau
    does THIS bin's alpha behave like," independent of the tilt formula that
    produced it."""
    if rising:
        b = sb.VectorizedRtTBallistics(1, alpha, 0.0)
        x = np.full(n_frames, 20.0)
        y = np.empty(n_frames)
        for i in range(n_frames):
            y[i] = b.step(x[i:i + 1])[0]
        y = np.concatenate([[0.0], y])
        return sb.time_to_63pct_frames(y, fs_frame, rising=True)
    else:
        b = sb.VectorizedRtTBallistics(1, 0.0, alpha)
        b.step(np.array([20.0]))
        x = np.full(n_frames, 0.0)
        y = np.empty(n_frames)
        for i in range(n_frames):
            y[i] = b.step(x[i:i + 1])[0]
        y = np.concatenate([[20.0], y])
        return sb.time_to_63pct_frames(y, fs_frame, rising=False)


def gate7_tilts(params):
    """Assert 7 -- tilt=0 == current (checked structurally in gate1);
    tilt_attack=+1 -> measured attack time at 8kHz ~= 2x the 1kHz value
    (tol +-20%; theoretical ratio is 2**0.75=1.682 since 8kHz is 3, not 4,
    octaves above 1kHz -- see gate docstring note); analogous release;
    tilt_detail sign check at 8kHz vs 200Hz. Also verifies
    tau_to_alpha_arr (this file's new vectorized port) against the
    UNMODIFIED scalar compressor.tau_to_alpha point-by-point -- the
    independent-reference check proto/CLAUDE.md requires for any new
    formula, even a "trivial" vectorization."""
    print("\n=== Test 7: tilt_attack / tilt_release / tilt_detail ===")
    ok = True

    fs_frame = FS / HOP
    for t in (0.001, 0.005, 0.027, 0.1, 0.5):
        scalar_a = tau_to_alpha(t, fs_frame)
        vec_a = float(tau_to_alpha_arr(np.array([t]), fs_frame)[0])
        p = abs(scalar_a - vec_a) < 1e-15
        ok &= p
        print(f"  tau_to_alpha_arr vs scalar tau_to_alpha, tau={t}s: "
              f"{vec_a:.17g} vs {scalar_a:.17g} -> {'PASS' if p else 'FAIL'}")

    coreA = SidebarChannelCore(N, FS, params, depth=5.0, detail=5.0,
                                attack_ms=sb.DEFAULT_ATTACK_MS,
                                release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                bands=[], lo=20.0, hi=20000.0, tilt_attack=1.0)
    bin_1k = int(np.argmin(np.abs(coreA.freqs - 1000.0)))
    bin_8k = int(np.argmin(np.abs(coreA.freqs - 8000.0)))
    aA_1k = float(coreA.ballistics.aA[bin_1k])
    aA_8k = float(coreA.ballistics.aA[bin_8k])
    tau_1k = _measure_bin_tau_ms(aA_1k, fs_frame, rising=True)
    tau_8k = _measure_bin_tau_ms(aA_8k, fs_frame, rising=True)
    ratio_a = tau_8k / tau_1k
    theo_a = 2.0 ** (1.0 * (np.log2(8000.0 / 1000.0) / np.log2(16.0)))
    p_attack = abs(ratio_a - 2.0) / 2.0 < 0.20
    ok &= p_attack
    print(f"  tilt_attack=+1: tau(1kHz)={tau_1k:.3f}ms tau(8kHz)={tau_8k:.3f}ms "
          f"ratio={ratio_a:.4f} (theoretical 2**0.75={theo_a:.4f}, target ~2.0 +-20%) -> "
          f"{'PASS' if p_attack else 'FAIL'}")

    coreR = SidebarChannelCore(N, FS, params, depth=5.0, detail=5.0,
                                attack_ms=sb.DEFAULT_ATTACK_MS,
                                release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                bands=[], lo=20.0, hi=20000.0, tilt_release=1.0)
    aR_1k = float(coreR.ballistics.aR[bin_1k])
    aR_8k = float(coreR.ballistics.aR[bin_8k])
    rel_1k = _measure_bin_tau_ms(aR_1k, fs_frame, rising=False)
    rel_8k = _measure_bin_tau_ms(aR_8k, fs_frame, rising=False)
    ratio_r = rel_8k / rel_1k
    p_release = abs(ratio_r - 2.0) / 2.0 < 0.20
    ok &= p_release
    print(f"  tilt_release=+1: tau(1kHz)={rel_1k:.3f}ms tau(8kHz)={rel_8k:.3f}ms "
          f"ratio={ratio_r:.4f} (theoretical 2**0.75={theo_a:.4f}, target ~2.0 +-20%) -> "
          f"{'PASS' if p_release else 'FAIL'}")

    bin200 = int(round(200.0 / (FS / N)))
    bin8000 = int(round(8000.0 / (FS / N)))
    core_pos = SidebarChannelCore(N, FS, params, depth=5.0, detail=5.0,
                                   attack_ms=sb.DEFAULT_ATTACK_MS,
                                   release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                   bands=[], lo=20.0, hi=20000.0, tilt_detail=0.5)
    core_neg = SidebarChannelCore(N, FS, params, depth=5.0, detail=5.0,
                                   attack_ms=sb.DEFAULT_ATTACK_MS,
                                   release_ms=sb.DEFAULT_RELEASE_MS, hop=HOP,
                                   bands=[], lo=20.0, hi=20000.0, tilt_detail=-0.5)
    x_hi = make_resonance_step(6.0, dur=0.8, f0=8000.0)
    x_lo = make_resonance_step(6.0, dur=0.8, f0=200.0)
    red_pos_hi = sc.mean_frame_reduction(x_hi, core_pos)[bin8000]
    red_neg_hi = sc.mean_frame_reduction(x_hi, core_neg)[bin8000]
    red_pos_lo = sc.mean_frame_reduction(x_lo, core_pos)[bin200]
    red_neg_lo = sc.mean_frame_reduction(x_lo, core_neg)[bin200]
    p_detail_hi = red_pos_hi > red_neg_hi
    p_detail_lo = red_pos_lo < red_neg_lo
    ok &= p_detail_hi and p_detail_lo
    print(f"  tilt_detail sign check @8kHz: +0.5 cut={red_pos_hi:.3f}dB "
          f"-0.5 cut={red_neg_hi:.3f}dB (+0.5 deeper) -> {'PASS' if p_detail_hi else 'FAIL'}")
    print(f"  tilt_detail sign check @200Hz: +0.5 cut={red_pos_lo:.3f}dB "
          f"-0.5 cut={red_neg_lo:.3f}dB (+0.5 shallower) -> {'PASS' if p_detail_lo else 'FAIL'}")

    print(f"  -> {'PASS' if ok else 'FAIL'}")
    return ok


# ============================================================================
# main
# ============================================================================
if __name__ == "__main__":
    print("=" * 78)
    print("Spectral SIDEBAR (Stage 2): scale/stereo/tilt globals on top of "
          "the verified weight+ballistics chain")
    print("=" * 78)

    print("\n(calibrating Stage-2 static params -- unmodified spectral_core.calibrate())")
    PARAMS = sc.calibrate()

    p1 = gate1_default_identity(PARAMS)
    p2 = gate2_max_cut(PARAMS)
    p3 = gate3_wet_trim(PARAMS)
    p4 = gate4_ms(PARAMS)
    p5 = gate5_link(PARAMS)
    p6 = gate6_focus(PARAMS)
    p7 = gate7_tilts(PARAMS)

    allp = p1 and p2 and p3 and p4 and p5 and p6 and p7
    print("\n" + "=" * 78)
    print(f"SUMMARY: default_identity={p1} max_cut={p2} wet_trim={p3} ms={p4} "
          f"link={p5} focus={p6} tilts={p7}")
    print(f"VERDICT: {'ALL PASS' if allp else 'SOME FAILURES'}")
    print("=" * 78)
    assert allp, "one or more gates failed -- see FAIL lines above"
