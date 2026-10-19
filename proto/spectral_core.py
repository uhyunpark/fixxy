"""Spectral processor, Stage 2: the CORE ALGORITHM of a soothe-class dynamic
resonance suppressor -- spectral ENVELOPE estimation -> relative EXCEED ->
proportional ATTENUATION (soft-knee gain computer) -> frequency-axis gain
SMOOTHING. NO time-domain ballistics yet (attack/release is Stage 3); every
frame is processed independently here, which is exactly why this stage is the
place to get the *static* transfer, the frequency selectivity, and the
level-independence right before time smoothing is layered on top.

WHY THIS DECOMPOSITION (docs/spectral-reference.md 4): soothe's audible identity
is NOT the skeleton (FFT + relative threshold + proportional cut -- all public
knowledge) but the TUNING of two things: how the spectral envelope (the
"background" a resonance is judged against) is estimated, and how the per-bin
gain is smoothed. This file pins both against real soothe3 measurements (the
target table in the module TARGETS dict, bounced from soothe3 in Stage 0).

THE CENTRAL FACT that makes a *relative* processor transparent on broadband
material (measured: white noise -> -0.14 dB, i.e. essentially untouched) while
still cutting a resonance hard: a coherent tone concentrates all its energy in
one FFT bin, whereas noise of the *same RMS* spreads its energy across ~N/2
bins. So a tone whose RMS merely EQUALS the noise RMS already stands ~28 dB
above the per-bin noise floor (measured here, N=1024: Delta = +28.2 dB). The
gain computer's knee therefore sits ~+20 dB up in true-exceed terms; random
noise bins, which exceed their own local envelope by only a few dB, fall far
below the knee and are left alone. This is the whole trick, and it is why the
envelope estimator must report the BACKGROUND (median-like, tone-rejecting),
not an average that the tone itself inflates.

RELATION TO Stage 1 (spectral_stft.py): that file proved the STFT->ISTFT round
trip is bit-exact under an arbitrary per-bin gain map, streaming, chunk-size
invariant, latency = N. This file injects a *per-frame, magnitude-dependent*
gain map into that proven pipeline via the smallest possible hook (a subclass
overriding one method -- ProcessingSTFT below; the base file is NOT modified).
Default framing: N=1024, hop=256, fs=44100 -> latency = N = 1024 samples =
23.2 ms. NOTE THE GAP: soothe3's low-latency class is ~2-5 ms at 44.1k; 23 ms
is much higher. That is a known Stage-4+ optimization topic (smaller N, or a
low-latency spectral mode), NOT a Stage-2 blocker -- Stage 2 is about getting
the transfer/selectivity math right, and those are framing-size-robust.

VERIFICATION PHILOSOPHY (proto/AGENTS.md): every claim is a numbered test with
the actual numbers and a defended tolerance; the grading references are real
soothe3 bounces (TARGETS), measured with the SAME estimators the Stage-0
analyzer used (tone_amp / welch_atten, imported from soothe3_measure_analyze so
the methodology cannot drift). Calibration (fitting the gain computer + detail
curves to TARGETS) is done with scipy.optimize; the fitted numbers printed by
this file ARE the C++ spec for Stage 4.
"""
import os

import numpy as np
from scipy.io import wavfile
from scipy.ndimage import median_filter
from scipy.optimize import least_squares

# Reuse the verified Stage-1 pipeline (windows + streaming STFT) and the Stage-0
# measurement methodology. Both modules are import-safe (work guarded by
# __main__); we do NOT modify either.
from spectral_stft import StreamingSTFT, sqrt_hann, cola_norm
from soothe3_measure_analyze import tone_amp, welch_atten

OUT = "proto/out"
os.makedirs(OUT, exist_ok=True)

# Default framing for this stage (see module docstring for the latency-gap note).
N = 1024
HOP = 256
FS = 44100

EPS = 1e-12


# ============================================================================
# THE MEASURED TARGET TABLE -- real soothe3 bounces (Stage 0). These are the
# grading references; a failed assert against them means the formula is wrong,
# not the tolerance (proto/AGENTS.md).
# ============================================================================
TARGETS = {
    # soft, Detail 5.0: tone exceed over noise floor (dB) -> attenuation (dB).
    # Monotonic, saturating. This is "the money plot" (exceed -> reduction).
    "transfer_exceed_db": np.array([-12, -6, 0, 6, 12, 18, 24], dtype=float),
    "transfer_atten_db": np.array([0.84, 2.68, 4.62, 6.12, 7.29, 8.13, 8.69]),
    # Detail knob -> (notch peak depth dB, notch FWHM Hz) on the notch_probe
    # (one loud 3 kHz tone on a broadband bed). Depth AND width move together.
    "detail_levels": np.array([0.0, 5.0, 10.0]),
    "detail_peak_db": np.array([2.9, 8.2, 13.0]),
    "detail_fwhm_hz": np.array([221.0, 323.0, 54.0]),
    # Level independence (soft): attenuation spread over a 36 dB input range.
    "level_indep_spread_db": 0.31,
    # White noise in soft mode is essentially untouched.
    "white_noise_atten_db": -0.14,
}

NOTCH_F0 = 3000.0  # the resonance frequency used by the resonance/notch probes


# ============================================================================
# PART 1: SPECTRAL ENVELOPE ESTIMATORS
#
# The envelope is the "background" spectral shape a bin is judged against. For a
# resonance suppressor the requirement is asymmetric and specific: on
# [sine + noise] the envelope must follow the NOISE FLOOR and NOT bulge up at
# the sine (otherwise the sine partly raises its own threshold and reads as less
# of a resonance); on a formant-structured spectrum it must hug the formants.
# We implement three and pick by data (test 1).
# ============================================================================

def _octave_bin_windows(freqs, halfwidth_oct):
    """For each bin, the [lo, hi] bin-index window spanning +/- halfwidth_oct
    octaves around that bin's frequency (constant-Q / octave-proportional: the
    window is a fixed fraction of an octave, so it is few bins wide at low
    frequency and many bins wide at high frequency). Bin 0 (DC) maps to itself.
    Precomputed once; the loop is over bins, not frames."""
    B = len(freqs)
    lo = np.zeros(B, dtype=int)
    hi = np.zeros(B, dtype=int)
    up = 2.0 ** halfwidth_oct
    for k in range(B):
        f = freqs[k]
        if f <= 0:
            lo[k], hi[k] = 0, 0
            continue
        flo, fhi = f / up, f * up
        # freqs are uniform (k*fs/N) -> invert directly.
        df = freqs[1]
        lo[k] = max(0, int(np.floor(flo / df)))
        hi[k] = min(B - 1, int(np.ceil(fhi / df)))
    return lo, hi


def envelope_boxcar(mag, lo, hi):
    """(a) Octave-proportional BOXCAR mean of the magnitude spectrum, via a
    prefix sum (this is exactly AnalyzerEngine's display-smoothing heritage --
    docs/spectral-reference.md 5). O(B) per frame. It AVERAGES the sine's energy
    into the local mean, so it BULGES at a tone -- quantified in test 1."""
    csum = np.concatenate([[0.0], np.cumsum(mag)])
    counts = (hi - lo + 1).astype(float)
    return (csum[hi + 1] - csum[lo]) / counts


def envelope_median_exact(mag, lo, hi):
    """(b) Octave-proportional frequency-axis MEDIAN. The median of a window
    dominated by broadband noise, with one narrow tone spike inside it, rejects
    the spike (a single outlier cannot move the 50th percentile of a wide
    window) -> the envelope tracks the noise floor, not the tone. This is the
    estimator we expect to win. Exact per-bin variable-window version, used for
    the single-frame comparison (test 1); the chain uses the fast log-resample
    version below."""
    B = len(mag)
    out = np.empty(B)
    for k in range(B):
        out[k] = np.median(mag[lo[k]:hi[k] + 1])
    return out


def envelope_cepstral(mag, n_lifter):
    """(c) CEPSTRAL liftering, the classic smooth spectral-envelope reference
    (docs/spectral-reference.md 8, CCRMA/JOS). Low-quefrency part of the log
    magnitude = the slowly-varying envelope. It is a SMOOTHING (like the
    boxcar in the log domain), so it also partly bulges at a strong tone -- kept
    as a reference point, not a candidate for the chain."""
    logmag = np.log(mag + EPS)
    # full spectrum log -> real cepstrum -> keep low quefrencies -> back.
    full = np.concatenate([logmag, logmag[-2:0:-1]])
    cep = np.fft.irfft(np.fft.rfft(full))
    lifter = np.zeros_like(cep)
    lifter[:n_lifter] = 1.0
    lifter[-n_lifter + 1:] = 1.0
    env_log = np.fft.irfft(np.fft.rfft(cep * lifter))[:len(mag)]
    return np.exp(env_log)


class FastOctaveMedian:
    """Octave-proportional median for the streaming chain, made cheap by working
    on a LOG-FREQUENCY resampling: resample |X| onto a uniform log-f grid (so a
    fixed-size median filter is automatically constant-Q / octave-proportional),
    median-filter, resample back to linear bins. O(B + M log k) per frame with a
    C-level median_filter, versus B separate np.median calls. The small
    interpolation error is irrelevant for a BACKGROUND estimate (verified: it
    still passes the tone-bulge criterion in test 1)."""

    def __init__(self, freqs, halfwidth_oct, M=512):
        self.freqs = freqs
        pos = freqs > 0
        self.pos = pos
        self.logf = np.log2(freqs[pos])
        self.grid = np.linspace(self.logf[0], self.logf[-1], M)
        octaves = self.logf[-1] - self.logf[0]
        pts_per_oct = M / octaves
        k = int(round(2.0 * halfwidth_oct * pts_per_oct))
        self.k = max(3, k | 1)  # odd, >= 3

    def __call__(self, mag):
        env = np.empty_like(mag)
        mpos = mag[self.pos]
        on_grid = np.interp(self.grid, self.logf, mpos)
        med = median_filter(on_grid, size=self.k, mode="nearest")
        env[self.pos] = np.interp(self.logf, self.grid, med)
        env[~self.pos] = mag[~self.pos]  # DC: pass through
        return env


# ============================================================================
# PART 2: GAIN COMPUTER (exceed -> reduction) + FREQUENCY-AXIS GAIN SMOOTHING
# ============================================================================

def reduction_db(exceed_db, max_db, knee_c, knee_w):
    """Soft-knee gain computer: exceed (bin dB above the local envelope) ->
    reduction (dB, >= 0). Smooth, monotonic, SATURATING toward max_db:

        R(e) = max_db * 0.5 * (1 + tanh((e - knee_c) / knee_w))

    tanh gives a smooth S-knee -- ~0 far below knee_c (so broadband noise, whose
    bins exceed their local envelope by only a few dB << knee_c, is left alone),
    rising through knee_c, saturating toward max_db far above. knee_c/knee_w are
    fixed by the measured transfer SHAPE; max_db is scaled by depth and detail.
    A smoothstep (Hermite) knee was the first candidate (task brief); tanh is
    used because it does not force zero slope at the ends -- the measured curve
    is still rising at +24 dB exceed (increment +0.56 dB), which a smoothstep
    that has already saturated cannot reproduce. Fit quality is reported in
    test 3."""
    return max_db * 0.5 * (1.0 + np.tanh((exceed_db - knee_c) / knee_w))


class OctaveSmoother:
    """Octave-proportional BOXCAR smoothing of the per-bin REDUCTION curve (dB),
    via prefix sum. Smoothing the reduction (not the gain) in dB keeps notch
    depth meaningful. A wider kernel spreads a resonance's cut over more
    neighbours AND shallows its peak (the cut's 'area' is conserved but
    redistributed) -- this single mechanism is why higher Detail is both deeper
    and narrower (narrow kernel -> tall sharp notch) and lower Detail shallower
    and broader. Width is octave-proportional so a notch is the same shape in
    octaves anywhere in the spectrum."""

    def __init__(self, freqs, width_oct):
        self.lo, self.hi = _octave_bin_windows(freqs, width_oct * 0.5)
        self.counts = (self.hi - self.lo + 1).astype(float)

    def __call__(self, red):
        csum = np.concatenate([[0.0], np.cumsum(red)])
        return (csum[self.hi + 1] - csum[self.lo]) / self.counts


# ============================================================================
# PART 3: DETAIL knob calibration curves
#
# Detail [0..10] drives TWO quantities at once (soothe3 folds sharpness +
# selectivity into this one knob): the gain-smoothing kernel WIDTH (octaves) and
# an overall DEPTH multiplier. The measured soothe3 data is NON-monotonic in
# width (FWHM 221 -> 323 -> 54 Hz at detail 0/5/10: it widens then sharply
# narrows) while depth rises monotonically (2.9 -> 8.2 -> 13.0 dB). A single
# monotonic law cannot reproduce that, so both curves are pinned at the three
# measured ANCHORS (detail 0/5/10) and log-linearly interpolated between; the
# depth multiplier is normalized to 1.0 at detail 5 so detail 5 == the gain
# computer's calibrated max_db (== the measured transfer). The anchor values
# (w0,w5,w10, m0,m10) are fit to the measured notch numbers in calibrate().
# ============================================================================

DETAIL_ANCHORS = np.array([0.0, 5.0, 10.0])


def _interp_detail(detail, vals):
    """Log-linear interpolation of `vals` (given at detail 0/5/10) to `detail`."""
    return float(np.exp(np.interp(detail, DETAIL_ANCHORS, np.log(vals))))


def detail_width_oct(detail, w0, w5, w10):
    return _interp_detail(detail, [w0, w5, w10])


def detail_depth_mult(detail, m0, m10):
    return _interp_detail(detail, [m0, 1.0, m10])


# ============================================================================
# PART 4: the SpectralCore processor (one frame in, one gain map out)
# ============================================================================

class SpectralCore:
    """Per-frame spectral gain computer. Stateless across frames by design
    (Stage 2 has NO time ballistics): frame_gain(mag) -> per-bin real gain.

    Parameters:
      depth  [0..10] : overall reduction scale; 5.0 == the measured curves;
                       0.0 == identity (all gains exactly 1.0 -> bit-exact null).
      detail [0..10] : sharpness+selectivity; sets kernel width AND depth mult.
    """

    def __init__(self, N, fs, params, depth=5.0, detail=5.0):
        self.freqs = np.fft.rfftfreq(N, d=1.0 / fs)
        self.p = params
        self.depth = depth
        self.detail = detail
        self.active = depth > 0.0
        # envelope estimator (chosen method: octave median), fixed window.
        self.env = FastOctaveMedian(self.freqs, params["env_halfwidth_oct"])
        # detail-dependent pieces, prepared once here (not per frame).
        self.width_oct = detail_width_oct(detail, params["w0"], params["w5"], params["w10"])
        self.smoother = OctaveSmoother(self.freqs, self.width_oct)
        self.dmult = detail_depth_mult(detail, params["m0"], params["m10"])
        self.max_db = params["max_db"] * (depth / 5.0) * self.dmult

    def frame_gain(self, mag):
        if not self.active:
            return np.ones_like(mag)
        env = self.env(mag)
        exceed_db = 20.0 * np.log10((mag + EPS) / (env + EPS))
        red = reduction_db(exceed_db, self.max_db, self.p["knee_c"], self.p["knee_w"])
        red = self.smoother(red)
        return 10.0 ** (-red / 20.0)


# ============================================================================
# PART 5: the smallest possible hook into the Stage-1 streaming pipeline
# ============================================================================

class ProcessingSTFT(StreamingSTFT):
    """StreamingSTFT with a per-frame, magnitude-dependent gain injected.

    The base class applies a STATIC per-bin gain array (`self.gain`); Stage 2
    needs the gain recomputed from each frame's own magnitude spectrum. The
    ONLY change from the base _process_one_hop is the three lines that build and
    apply the gain (marked HOOK below); every byte of the OLA / latency /
    position-indexing logic is copied verbatim from spectral_stft.py so the
    proven latency = N, chunk-invariance, and bit-exact-null properties carry
    over unchanged (the base file is intentionally NOT modified). depth == 0
    (core.active False) leaves the spectrum untouched -> pure identity."""

    def __init__(self, N, hop, fs, core):
        super().__init__(N, hop, fs, gain_fn=None)
        self.core = core

    def _process_one_hop(self):
        chunk = np.array([self.in_fifo.popleft() for _ in range(self.hop)])
        self.input_ring = np.concatenate([self.input_ring[self.hop:], chunk])

        frame = self.input_ring * self.analysis_win
        spec = np.fft.rfft(frame)
        # ---- HOOK: per-frame magnitude-dependent gain --------------------
        if self.core.active:
            gain = self.core.frame_gain(np.abs(spec))
            spec = spec * gain
        # ------------------------------------------------------------------
        frame_out = np.fft.irfft(spec, self.N) * self.synth_win

        self.ola_buffer += frame_out
        ready = self.ola_buffer[: self.hop] * self.norm
        self.ola_buffer = np.concatenate([self.ola_buffer[self.hop:], np.zeros(self.hop)])

        self.hops_done += 1
        start = self.hops_done * self.hop - self.N
        if start >= 0:
            self._ensure_out_buf(start + self.hop)
            self.out_buf[start: start + self.hop] = ready


def run_chain(x, params, depth=5.0, detail=5.0):
    """Feed x through the streaming pipeline with a SpectralCore; return output
    aligned to the input (the leading latency = N samples dropped, trimmed to
    len(x))."""
    core = SpectralCore(N, FS, params, depth=depth, detail=detail)
    stft = ProcessingSTFT(N, HOP, FS, core)
    out = np.concatenate([stft.process(x), stft.flush()])
    return out[N: N + len(x)]


# ============================================================================
# PART 6: probe signal builders (self-contained equivalents of the Stage-0 kit)
# ============================================================================

_RNG = np.random.default_rng(20260810)


def _db2amp(db):
    return 10.0 ** (db / 20.0)


def _sine_rms(freq, dur, rms_dbfs, fs=FS):
    n = int(round(dur * fs))
    t = np.arange(n) / fs
    return _db2amp(rms_dbfs) * np.sqrt(2.0) * np.sin(2.0 * np.pi * freq * t)


def _noise_rms(dur, rms_dbfs, fs=FS):
    n = int(round(dur * fs))
    x = _RNG.standard_normal(n)
    return x * _db2amp(rms_dbfs) / np.sqrt(np.mean(x ** 2))


def make_resonance_step(rel_db, dur=0.6, noise_dbfs=-36.0, f0=NOTCH_F0):
    """One resonance_in_noise step: a fixed-RMS white-noise floor + a tone whose
    RMS is rel_db above the floor. (Same construction as
    soothe3_measure_gen.build_resonance_in_noise, one step.)"""
    tone = _sine_rms(f0, dur, noise_dbfs + rel_db)
    floor = _noise_rms(dur, noise_dbfs)[:len(tone)]
    return tone + floor


def make_notch_probe(dur=4.0, tone_dbfs=-18.0, bed_dbfs=-36.0, f0=NOTCH_F0):
    """One loud tone on a broadband bed (soothe3_measure_gen.build_notch_probe).
    The bed makes the notch's shoulders visible at every frequency, so
    welch_atten(in,out) around f0 reveals the kernel width directly."""
    tone = _sine_rms(f0, dur, tone_dbfs)
    bed = _noise_rms(dur, bed_dbfs)[:len(tone)]
    return tone + bed


def measure_tone_atten(x_in, x_out, f0, guard=0.15):
    """Attenuation (dB) of the f0 tone, via the Stage-0 single-frequency DFT
    projection (tone_amp) over the steady middle of the signal. tone_amp
    isolates the coherent tone from the surrounding noise, so this reads the
    resonance's attenuation, not the bed's."""
    g = int(guard * FS)
    a_in = tone_amp(x_in[g:-g], f0, FS)
    a_out = tone_amp(x_out[g:-g], f0, FS)
    return 20.0 * np.log10(a_out / a_in)


# ============================================================================
# PART 6b: FAST measurement via per-frame gains (no OLA).
#
# For a STEADY signal the OLA output attenuation equals the per-frame gain the
# core computes (verified: matches the tone_amp-through-OLA transfer within
# ~0.1 dB at all 7 points). So calibration -- which needs hundreds of chain
# evaluations -- is done on the per-frame gain directly, skipping the OLA and
# the per-sample output loop. The GATES still measure the real streaming chain.
# ============================================================================
_WIN = sqrt_hann(N)
_FREQS = np.fft.rfftfreq(N, d=1.0 / FS)
_TONE_BIN = int(round(NOTCH_F0 / (FS / N)))


def mean_frame_reduction(x, core):
    """Average per-bin reduction (dB, >= 0) the core applies across all frames
    of x. This is the frequency KERNEL for a steady probe."""
    reds = []
    for i in range(0, len(x) - N, HOP):
        mag = np.abs(np.fft.rfft(x[i:i + N] * _WIN))
        reds.append(-20.0 * np.log10(core.frame_gain(mag)))
    return np.mean(reds, axis=0)


_CAL_STEPS = [make_resonance_step(r, dur=0.6) for r in TARGETS["transfer_exceed_db"]]


def ola_transfer(params):
    """The 7-point exceed->reduction transfer measured through the REAL OLA
    chain (tone_amp on the output), depth 5/detail 5 -- the same quantity the
    gate checks. Calibrating on this (not the per-frame peak) is required
    because at a wide detail-5 smoothing kernel the tone's effective OLA
    attenuation is ~1 dB shallower than the peak per-bin reduction: the tone's
    energy spreads across bins whose reduction the wide kernel has flattened."""
    out = []
    for s in _CAL_STEPS:
        y = run_chain(s, params, depth=5.0, detail=5.0)
        out.append(-measure_tone_atten(s, y, NOTCH_F0))
    return np.array(out)


def _fwhm_interp(freqs, red):
    """Peak reduction and FWHM (Hz) of a notch in `red` vs `freqs`, with linear
    interpolation of the half-max crossings (sub-bin -> smooth for the fitter)."""
    m = (freqs > NOTCH_F0 / 2.0) & (freqs < NOTCH_F0 * 2.0)
    fa, aa = freqs[m], red[m]
    ipk = int(np.argmax(aa))
    pk = float(aa[ipk])
    half = pk / 2.0

    def cross(i, j):  # interpolate freq where aa crosses `half` between i and j
        if aa[i] == aa[j]:
            return fa[i]
        return fa[i] + (half - aa[i]) * (fa[j] - fa[i]) / (aa[j] - aa[i])

    lo = ipk
    while lo > 0 and aa[lo - 1] > half:
        lo -= 1
    hi = ipk
    while hi < len(aa) - 1 and aa[hi + 1] > half:
        hi += 1
    f_lo = cross(lo, lo - 1) if lo > 0 else fa[lo]
    f_hi = cross(hi, hi + 1) if hi < len(aa) - 1 else fa[hi]
    return pk, float(f_hi - f_lo)


_WHITE_CAL = _noise_rms(1.0, -20.0)
_BAND = (_FREQS > 100) & (_FREQS < 18000)


def fast_white_reduction(params):
    """Broadband mean per-frame reduction (dB) on white noise, depth 5/detail 5.
    A relative processor should leave broadband noise alone; used as a
    calibration objective so the fit prefers the transparent corner of the
    transfer-fit space (the soft knee has some freedom -- see calibrate())."""
    core = SpectralCore(N, FS, params, depth=5.0, detail=5.0)
    red = mean_frame_reduction(_WHITE_CAL, core)
    return float(np.mean(red[_BAND]))


def fast_kernel(params, detail):
    """Notch (peak dB, FWHM Hz) on the notch_probe from per-frame gains -- the
    true frequency kernel, free of the Welch/pure-tone measurement artifacts (a
    sub-bin dip at the tone that truncates a Welch half-max walk). Note test 4:
    this matches soothe3's Welch FWHM for detail 0/5, and exposes the N=1024
    resolution floor at detail 10."""
    core = SpectralCore(N, FS, params, depth=5.0, detail=detail)
    red = mean_frame_reduction(make_notch_probe(dur=1.5), core)
    return _fwhm_interp(_FREQS, red)


# ============================================================================
# PART 7: CALIBRATION -- fit the gain computer + detail curves to TARGETS.
# The numbers this prints ARE the C++ spec (Stage 4).
# ============================================================================

def calibrate():
    """JOINT fit of the gain computer (max_db, knee_c, knee_w) AND the detail
    anchors (w0, w5, m0, m10) against the transfer (7 pts) + the detail peaks
    (3) + the detail-0/5 FWHM (2), all via the fast per-frame measurement.

    Why joint: the transfer is measured at detail 5, whose smoothing width (w5)
    shallows a single tone's cut -- so max_db and w5 are coupled and cannot be
    fit in sequence. detail 10's width is NOT a free variable: it is pinned to
    the resolution floor (w10 = 0.005 oct, effectively no smoothing) because
    soothe3's 54 Hz detail-10 FWHM is BELOW what N=1024 can resolve (the sqrt-
    Hann main lobe alone is ~129 Hz wide -- see test 4); only detail-10 DEPTH
    (via m10) is fit."""
    params = {
        "env_halfwidth_oct": 0.5,   # 1-octave median window (justified test 1)
        "max_db": 12.0, "knee_c": 19.0, "knee_w": 10.0,
        "w0": 0.05, "w5": 0.13, "w10": 0.005, "m0": 0.35, "m10": 1.2,
    }
    tgt_atten = TARGETS["transfer_atten_db"]
    tgt_pk = TARGETS["detail_peak_db"]
    tgt_w = TARGETS["detail_fwhm_hz"]

    # theta = [max_db, knee_c, knee_w, w0, w5, m0, m10]
    def resid(theta):
        p = dict(params, max_db=theta[0], knee_c=theta[1], knee_w=theta[2],
                 w0=theta[3], w5=theta[4], m0=theta[5], m10=theta[6])
        tr = ola_transfer(p) - tgt_atten                        # 7, in dB
        k = [fast_kernel(p, d) for d in DETAIL_ANCHORS]         # (peak, fwhm) x3
        pk = np.array([kk[0] for kk in k]) - tgt_pk             # 3, in dB
        wd = 3.0 * (np.array([k[0][1], k[1][1]]) - tgt_w[:2]) / tgt_w[:2]  # 2, weighted rel
        # white-noise transparency objective (target 0), weighted so it breaks
        # ties toward the transparent corner without overriding the transfer.
        wn = np.array([2.0 * fast_white_reduction(p)])          # 1
        return np.concatenate([tr, pk, wd, wn])

    # NOTE the w0/w5 lower bounds (0.04 / 0.05 oct): below ~0.04 oct the notch
    # FWHM saturates at the N=1024 resolution floor, where it no longer responds
    # to the width parameter -> the width residual's gradient vanishes and a
    # gradient fitter would park w0 at the floor (satisfying the peak via m0
    # alone) and miss the width target. Keeping w0/w5 in the width-responsive
    # region forces the fit to honour the measured FWHMs. (detail 10 is the one
    # anchor deliberately AT the floor -- w10 is fixed, not fit.)
    r = least_squares(
        resid, [12.0, 19.0, 10.0, 0.05, 0.09, 0.35, 1.2],
        bounds=([2, 0, 1, 0.04, 0.05, 0.05, 0.5],
                [40, 40, 30, 0.20, 0.40, 1.0, 4.0]), diff_step=0.03)
    (params["max_db"], params["knee_c"], params["knee_w"],
     params["w0"], params["w5"], params["m0"], params["m10"]) = r.x
    return params


# ============================================================================
# PART 8: GATES
# ============================================================================

def gate1_envelope_comparison():
    """Test 1 -- envelope method comparison. On [sine + noise] the envelope must
    follow the noise floor and NOT bulge at the sine. We quantify the BULGE =
    (envelope dB at the tone bin) - (true noise-floor dB there), for boxcar,
    exact octave-median, the fast log-resample median, and cepstral. Lower is
    better; the median should be ~0."""
    print("\n=== Test 1: envelope method comparison (tone-bulge, dB) ===")
    freqs = np.fft.rfftfreq(N, d=1.0 / FS)
    w = sqrt_hann(N)
    f0 = round(NOTCH_F0 / (FS / N)) * (FS / N)
    kb = int(round(f0 / (FS / N)))
    t = np.arange(N) / FS
    lo, hi = _octave_bin_windows(freqs, 0.5)
    fmed = FastOctaveMedian(freqs, 0.5)
    # tone RMS == noise RMS (rel 0): a modest tone, so any bulge is the
    # estimator's fault, not an overwhelming one. AVERAGE the bulge over many
    # independent fixed-seed noise frames -- a single 1024-sample noise frame is
    # too high-variance to grade on (the median of an ~80-bin window still
    # wobbles a few dB frame to frame); the mean bulge is the estimator property.
    rng = np.random.default_rng(1234)
    names = ["boxcar (a)", "median-exact (b)", "median-fast (b')", "cepstral (c)"]
    acc = {n: [] for n in names}
    for _ in range(48):
        tone = np.sqrt(2.0) * np.sin(2.0 * np.pi * f0 * t)
        noise = rng.standard_normal(N)
        noise *= 1.0 / np.sqrt(np.mean(noise ** 2))
        mag = np.abs(np.fft.rfft((tone + noise) * w))
        mag_noise = np.abs(np.fft.rfft(noise * w))
        floor_db = 20.0 * np.log10(np.median(mag_noise[lo[kb]:hi[kb] + 1]) + EPS)
        envs = {
            "boxcar (a)": envelope_boxcar(mag, lo, hi),
            "median-exact (b)": envelope_median_exact(mag, lo, hi),
            "median-fast (b')": fmed(mag),
            "cepstral (c)": envelope_cepstral(mag, n_lifter=40),
        }
        for n in names:
            acc[n].append(20.0 * np.log10(envs[n][kb] + EPS) - floor_db)
    bulges = {n: float(np.mean(acc[n])) for n in names}
    for name in names:
        print(f"  {name:20s} mean bulge at tone = {bulges[name]:6.2f} dB "
              f"(std {np.std(acc[name]):.2f})")
    chosen = "median-fast (b')"
    ok = abs(bulges["median-exact (b)"]) < 1.0 and abs(bulges[chosen]) < 1.5 \
        and bulges["boxcar (a)"] > 3.0  # boxcar demonstrably bulges
    print(f"  -> chosen for chain: {chosen} (bulge {bulges[chosen]:.2f} dB); "
          f"boxcar bulges {bulges['boxcar (a)']:.2f} dB (rejected)")
    print(f"  {'PASS' if ok else 'FAIL'}")
    return ok


def gate2_level_independence(params):
    """Test 2 -- level independence. Run the whole chain on the same resonance
    (tone 12 dB over its noise floor) at input levels -42..-6 dBFS; the tone
    attenuation must barely change (relative design). Gate: spread < 0.5 dB
    (soothe measured 0.31)."""
    print("\n=== Test 2: level independence (atten spread over 36 dB) ===")
    attens = []
    base = make_resonance_step(12.0, dur=0.6)
    levels = list(range(-42, -5, 6))  # -42..-6, 7 levels (36 dB span)
    for lvl in levels:
        g = _db2amp(lvl - (-24.0))  # -24 dBFS is the nominal reference level
        s = base * g
        y = run_chain(s, params, depth=5.0, detail=5.0)
        a = measure_tone_atten(s, y, NOTCH_F0)
        attens.append(a)
        print(f"  input {lvl:+4d} dBFS -> atten {a:5.2f} dB")
    spread = max(attens) - min(attens)
    ok = spread < 0.5
    print(f"  spread = {spread:.3f} dB (gate <0.5, soothe 0.31) -> {'PASS' if ok else 'FAIL'}")
    return ok


def gate3_transfer(params):
    """Test 3 -- static exceed->reduction transfer vs the 7 measured points,
    measured through the REAL streaming OLA chain (tone_amp on the output, the
    Stage-0 way). Tolerance: RMS error < 1.0 dB (defensible: the measured points
    carry ~0.1 dB read noise, and the 6 dB step spacing means <1 dB RMS is a
    tight shape match); every per-point |error| reported."""
    print("\n=== Test 3: exceed->reduction transfer vs measured 7 points "
          "(through OLA chain) ===")
    rel = TARGETS["transfer_exceed_db"]
    tgt = TARGETS["transfer_atten_db"]
    got = []
    for r in rel:
        s = make_resonance_step(r, dur=0.8)
        y = run_chain(s, params, depth=5.0, detail=5.0)
        got.append(-measure_tone_atten(s, y, NOTCH_F0))
    got = np.array(got)
    err = got - tgt
    for e_db, g, t_, er in zip(rel, got, tgt, err):
        print(f"  exceed {e_db:+4.0f} dB: got {g:5.2f}  target {t_:5.2f}  err {er:+5.2f}")
    rms = float(np.sqrt(np.mean(err ** 2)))
    ok = rms < 1.0
    print(f"  RMS error = {rms:.3f} dB (gate <1.0) -> {'PASS' if ok else 'FAIL'}")
    return ok


def gate4_detail(params):
    """Test 4 -- detail (depth, width) at detail {0,5,10} vs measured pairs.

    Width is the TRUE frequency kernel (per-bin gain averaged over frames), not
    a Welch-on-output FWHM: measuring a pure-tone notch with Welch has a sub-bin
    dip artifact at the tone that truncates the half-max walk, AND a resolution
    floor -- both measurement noise, not the algorithm. The true kernel is the
    frequency selectivity actually applied and is what the C++ spec needs; it
    also happens to match soothe3's Welch FWHM at detail 0/5 (215~221, 301~323).

    Tolerances: depth within 1.5 dB, FWHM within 25% at detail 0 and 5. DETAIL
    10 IS A DOCUMENTED PHYSICAL LIMIT: soothe3's 54 Hz FWHM is below what N=1024
    can resolve -- with zero smoothing the sqrt-Hann main lobe alone gives a
    ~129 Hz notch (the floor, measured here). So at detail 10 we assert the
    DEPTH matches and the width is at the floor (narrowest achievable, i.e.
    < detail-5's), and flag the 54 Hz target as a Stage-4 larger-N / quality-
    mode item -- the same frequency-resolution/latency tradeoff as the 23 ms
    latency gap noted at the top of this file."""
    print("\n=== Test 4: detail calibration (peak dB, FWHM Hz; true kernel) ===")
    # measure the resolution floor (no smoothing).
    floor_p = dict(params, w0=0.005, w5=0.005, w10=0.005)
    _, floor_hz = fast_kernel(floor_p, 5.0)
    print(f"  N={N} resolution floor (zero-smoothing FWHM) = {floor_hz:.0f} Hz "
          f"-> soothe3's 54 Hz detail-10 notch is UNREACHABLE at this frame size "
          f"(Stage-4 larger-N item)")
    ok = True
    fwhms = {}
    for d, tp, tw in zip(TARGETS["detail_levels"], TARGETS["detail_peak_db"],
                         TARGETS["detail_fwhm_hz"]):
        pk, wd = fast_kernel(params, d)
        fwhms[d] = wd
        pk_ok = abs(pk - tp) < 1.5
        if d < 10:
            w_ok = abs(wd - tw) / tw < 0.25
            note = f"tgt {tw:5.1f}, {'ok' if w_ok else 'X'}"
        else:
            # detail 10: assert at floor + narrowest, not the unreachable 54 Hz.
            w_ok = wd <= 1.25 * floor_hz and wd < fwhms[5.0]
            note = (f"tgt {tw:5.1f} UNREACHABLE; at floor & narrowest "
                    f"{'ok' if w_ok else 'X'}")
        ok &= pk_ok and w_ok
        print(f"  detail {d:4.1f}: peak {pk:5.2f} (tgt {tp:4.1f}, {'ok' if pk_ok else 'X'})  "
              f"FWHM {wd:6.1f} Hz ({note})")
    print(f"  -> {'PASS' if ok else 'FAIL'}")
    return ok


def gate5_white_noise(params):
    """Test 5 -- white-noise transparency (soft). Full chain on 4 s of white
    noise; broadband mean attenuation must be < 0.5 dB (soothe measured -0.14).
    Measured as the broadband Welch power ratio, ignoring the outermost bins."""
    print("\n=== Test 5: white-noise transparency ===")
    x = _noise_rms(4.0, -20.0)
    y = run_chain(x, params, depth=5.0, detail=5.0)
    f, atten = welch_atten(x, y, FS, nperseg=4096)
    band = (f > 100) & (f < 18000)
    mean_atten = float(np.mean(atten[band]))
    ok = abs(mean_atten) < 0.5
    print(f"  broadband mean attenuation = {mean_atten:.3f} dB "
          f"(gate <0.5, soothe -0.14) -> {'PASS' if ok else 'FAIL'}")
    return ok


def gate6_selectivity(params):
    """Test 6 -- tone-vs-resonance selectivity. A musical chord (3 notes x 8
    harmonics, 1/h rolloff) forms a real harmonic spectral envelope; one extra
    INHARMONIC 'resonance' tone is 12 dB hotter than the loudest chord partial.
    The resonance must attenuate >= 4x more dB than any chord partial.

    WHY the chord is dense with harmonic rolloff (not a few tones on a silent
    bed): a relative processor judges a bin against its LOCAL ENVELOPE. On a
    silent bed every tone -- partial or resonance -- towers ~28 dB over the
    per-bin floor and all saturate to the same reduction (selectivity ~1x). Only
    when the surrounding musical content raises the local envelope do the
    balanced chord partials read as low-exceed (part of the spectrum, left
    alone) while the hotter, isolated, inharmonic resonance still stands out and
    is cut. The resonance is placed off the harmonic grid so it does not merely
    reinforce a partial; partials within +-0.15 oct of it (its kernel spill) are
    excluded from the 'worst partial' read."""
    print("\n=== Test 6: tone-vs-resonance selectivity ===")
    dur = 1.5
    rng = np.random.default_rng(7)
    t = np.arange(int(dur * FS)) / FS
    notes = [130.81, 164.81, 196.00]  # C3 E3 G3
    partials = []
    x = rng.standard_normal(len(t))
    x *= _db2amp(-45.0) / np.sqrt(np.mean(x ** 2))  # quiet broadband bed
    part_amp = 0.1
    for nf in notes:
        for h in range(1, 9):
            f = nf * h
            if f < 18000:
                x = x + (part_amp / h) * np.sin(2 * np.pi * f * t)
                partials.append(f)
    peak_partial_amp = part_amp  # h=1 partial is the loudest
    # resonance: inharmonic, 12 dB above the loudest chord partial.
    f_res = 2637.0
    x = x + peak_partial_amp * _db2amp(12.0) * np.sin(2 * np.pi * f_res * t)
    x *= 0.6 / np.max(np.abs(x))
    y = run_chain(x, params, depth=5.0, detail=5.0)

    res_atten = -measure_tone_atten(x, y, f_res)
    part_attens = [-measure_tone_atten(x, y, f) for f in partials
                   if abs(np.log2(f / f_res)) > 0.15]
    worst_partial = max(part_attens)
    ratio = res_atten / max(worst_partial, 1e-3)
    ok = ratio >= 4.0
    print(f"  resonance ({f_res:.0f} Hz, +12 dB) reduction = {res_atten:.2f} dB")
    print(f"  worst chord-partial reduction = {worst_partial:.2f} dB")
    print(f"  ratio = {ratio:.1f}x (gate >=4x) -> {'PASS' if ok else 'FAIL'}")
    return ok


def gate7_null():
    """Test 7 -- depth = 0 is a bit-exact null. With core.active False the
    pipeline is the Stage-1 pure identity, so output == input delayed by N to
    < 1e-10 (same claim Stage 1's passthrough_null makes)."""
    print("\n=== Test 7: depth=0 bit-exact null ===")
    x = _noise_rms(1.0, -20.0)
    y = run_chain(x, PARAMS, depth=0.0, detail=5.0)
    diff = float(np.max(np.abs(y - x)))
    ok = diff < 1e-10
    print(f"  max|out - in| = {diff:.2e} (gate <1e-10) -> {'PASS' if ok else 'FAIL'}")
    return ok


def gate8_renders(params):
    """Test 8 -- render dry/processed/DELTA WAVs (the delta = what we removed is
    the primary listening tool) for (a) the resonance-in-noise probe and (b) a
    synthesized harsh-vocal-ish signal (bright resonant peak ~3.3 kHz on a
    harmonic bed)."""
    print("\n=== Test 8: renders ===")
    written = []

    # (a) resonance in noise (concatenated steps, like the Stage-0 probe).
    a_dry = np.concatenate([make_resonance_step(r, dur=1.2)
                            for r in (-6, 6, 18)])
    a_wet = run_chain(a_dry, params, depth=5.0, detail=5.0)
    for tag, sig in (("dry", a_dry), ("processed", a_wet), ("delta", a_dry - a_wet)):
        path = f"{OUT}/spectral_core_resonance_{tag}.wav"
        wavfile.write(path, FS, (0.9 * sig / max(1e-9, np.max(np.abs(a_dry)))).astype(np.float32))
        written.append(path)

    # (b) harsh-vocal-ish: harmonic bed (100 Hz f0) + a hot resonant peak at
    # ~3.3 kHz (the classic sibilant/harsh region) rising above it.
    dur = 4.0
    t = np.arange(int(dur * FS)) / FS
    f0v = 100.0
    b = np.zeros_like(t)
    for h in range(1, 40):
        f = f0v * h
        if f < 18000:
            b += (1.0 / h) * np.sin(2 * np.pi * f * t)  # 1/h harmonic rolloff
    b += 0.3 * _noise_rms(dur, -30.0)[:len(t)]
    # resonant peak: narrowband energy centred at 3.3 kHz, ~12 dB above the bed.
    fr = 3300.0
    res = 6.0 * np.sin(2 * np.pi * fr * t) * (0.6 + 0.4 * np.sin(2 * np.pi * 4 * t))
    x = b + res
    x *= 0.7 / np.max(np.abs(x))
    y = run_chain(x, params, depth=6.0, detail=6.0)
    for tag, sig in (("dry", x), ("processed", y), ("delta", x - y)):
        path = f"{OUT}/spectral_core_vocal_{tag}.wav"
        wavfile.write(path, FS, sig.astype(np.float32))
        written.append(path)

    for p in written:
        print(f"  wrote {p}")
    return True


# ============================================================================
# main
# ============================================================================
PARAMS = None

if __name__ == "__main__":
    print("=" * 78)
    print("Spectral core (Stage 2): envelope -> exceed -> gain computer -> "
          "freq smoothing")
    print(f"  framing N={N} hop={HOP} fs={FS} -> latency={N} samples "
          f"({N / FS * 1000:.1f} ms)")
    print("=" * 78)

    print("\n=== Calibration (joint fit of gain computer + detail anchors to "
          "soothe3 measurements) ===")
    PARAMS = calibrate()
    print("  --- fitted GAIN COMPUTER (the C++ spec) ---")
    print(f"    max_db (detail5,depth5) = {PARAMS['max_db']:.3f}")
    print(f"    knee_c (true-exceed dB) = {PARAMS['knee_c']:.3f}")
    print(f"    knee_w (dB)             = {PARAMS['knee_w']:.3f}")
    print(f"    reduction_db(e) = max_db * 0.5*(1 + tanh((e - knee_c)/knee_w))")
    print("  --- fitted DETAIL anchors (log-linear interp over detail 0/5/10) ---")
    print(f"    width_oct  @ 0/5/10 = {PARAMS['w0']:.4f} / {PARAMS['w5']:.4f} / "
          f"{PARAMS['w10']:.4f} oct   (w10 pinned at resolution floor)")
    print(f"    depth_mult @ 0/5/10 = {PARAMS['m0']:.3f} / 1.000 / {PARAMS['m10']:.3f}")
    print(f"    env_halfwidth_oct = {PARAMS['env_halfwidth_oct']} (octave median)")

    p1 = gate1_envelope_comparison()
    p2 = gate2_level_independence(PARAMS)
    p3 = gate3_transfer(PARAMS)
    p4 = gate4_detail(PARAMS)
    p5 = gate5_white_noise(PARAMS)
    p6 = gate6_selectivity(PARAMS)
    p7 = gate7_null()
    p8 = gate8_renders(PARAMS)

    allp = p1 and p2 and p3 and p4 and p5 and p6 and p7 and p8
    print("\n" + "=" * 78)
    print(f"SUMMARY: envelope={p1} level_indep={p2} transfer={p3} detail={p4} "
          f"white_noise={p5} selectivity={p6} null={p7} renders={p8}")
    print(f"VERDICT: {'ALL PASS' if allp else 'SOME FAILURES'}")
    print("=" * 78)
    assert allp, "one or more gates failed -- see FAIL lines above"
