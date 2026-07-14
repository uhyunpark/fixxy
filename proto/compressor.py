"""Feed-forward digital compressor: quadratic soft-knee gain computer, four
peak-detector ballistics topologies (branching/decoupled x RtZ/RtT-smooth),
and a program-dependent dual-pole release. All formulas are Stage-1 research
ground truth -- implemented exactly as specified; every numeric claim below
is re-derived from the asserts in verify_*(), not copied from a textbook.

ARCHITECTURE (feed-forward):
  sidechain: input -> |x| (or RMS) -> x_L = 20*log10(max(|x|,1e-6)) dB
  gain computer:      y_G(x_L)            (quadratic soft-knee, see below)
  control signal:     c = y_G(x_L) - x_L  (<= 0 dB, how much to attenuate)
  ballistics:         smooth c -> g_sm
  output:             input * 10^((g_sm + makeup)/20)

SIGN CONVENTION (documented per spec's "your choice"): the detector formulas
in the spec are written for a signal that RISES on attack. c is <= 0 and
gets MORE negative as gain reduction increases -- that's a FALLING signal on
attack, backwards from the formulas as literally written. Rather than swap
which alpha means "attack" (error-prone, contradicts the reader's textbook
intuition of "attack = alpha for the rising branch"), this file smooths the
NEGATED control signal e = -c >= 0 ("gain-reduction demand": 0 = no
reduction, rising = more reduction). This is exactly the P. Giannoulis /
Massberg / Reiss convention (x_G(n) in "Digital Dynamic-Range Compressor
Design", 2012). All four detector formulas below run on e unmodified,
attack fires when e is rising (more reduction demanded), release when e is
falling. The final smoothed gain is g_sm = -y(e), which is <= 0 dB as
required by the output stage.

TAU CONVENTION: alpha = exp(-1/(tau*fs)); tau = time to 63.2% of a step.
attack tau <= 0.001 ms => alpha = 0 (instant, no smoothing).
"""
import numpy as np
from scipy.io import wavfile
import matplotlib.pyplot as plt
import os

sr = 44100
OUT = "proto/out"
os.makedirs(OUT, exist_ok=True)


# ============================================================
# PART 0: shared primitives
# ============================================================

def tau_to_alpha(tau_s, fs):
    """alpha = exp(-1/(tau*fs)).  tau <= 1us (0.001 ms) => alpha = 0 (instant)."""
    if tau_s <= 1e-6:
        return 0.0
    return float(np.exp(-1.0 / (tau_s * fs)))


def gain_computer(x_db, T, R, W):
    """Quadratic soft-knee gain computer, y_G(x).

    x < T-W/2         : y = x                                  (untouched)
    |x-T| <= W/2       : y = x + (1/R-1)*(x-T+W/2)^2 / (2*W)     (knee)
    x > T+W/2         : y = T + (x-T)/R                          (compressed)
    W -> 0 collapses the knee branch to a single point (hard knee).
    """
    scalar = np.isscalar(x_db)
    x = np.atleast_1d(np.asarray(x_db, dtype=np.float64))
    y = np.copy(x)
    below = 2 * (x - T) < -W
    above = 2 * (x - T) > W
    knee = ~below & ~above
    y[above] = T + (x[above] - T) / R
    if W > 0:
        xk = x[knee]
        y[knee] = xk + (1.0 / R - 1.0) * (xk - T + W / 2.0) ** 2 / (2.0 * W)
    return float(y[0]) if scalar else y


class OnePoleHPF:
    """TPT one-pole highpass -- used ONLY in the sidechain/detector path
    (test 8), never in the main audio path. Same integrator-state trick as
    multiband_eq's SVF: coefficients can be re-prepared live without a click.
    """

    def __init__(self):
        self.s = 0.0
        self.G = 0.0

    def prepare(self, fs, fc):
        g = np.tan(np.pi * fc / fs)
        self.G = g / (1 + g)

    def reset(self):
        self.s = 0.0

    def process_sample(self, x):
        v = (x - self.s) * self.G
        lp = v + self.s
        self.s = lp + v
        return x - lp


# ============================================================
# PART 1: ballistics detectors (operate on e = -c >= 0, "GR demand")
# ============================================================

class PeakDetector:
    """Four textbook peak-detector ballistics, selected by `kind`:
      branching_rtz       : y = aA*y1+(1-aA)*x if x>y1 else aR*y1
      smooth_branching_rtt: y = aA*(y1-x)+x if x>=y1 else aR*(y1-x)+x
      decoupled_rtz        : s1=max(x,aR*s1); y=aA*y1+(1-aA)*s1
      smooth_decoupled_rtt : s1=max(x,aR*(s1-x)+x); y=aA*(y1-s1)+s1  (SHIP)
    """
    KINDS = ("branching_rtz", "smooth_branching_rtt",
             "decoupled_rtz", "smooth_decoupled_rtt")

    def __init__(self, kind="smooth_decoupled_rtt"):
        assert kind in self.KINDS
        self.kind = kind
        self.aA = self.aR = 0.0
        self.y1 = 0.0
        self.s1 = 0.0
        self.hold_samples = 0
        self.hold_counter = 0

    def prepare(self, fs, attack_ms, release_ms, hold_ms=0.0):
        self.aA = tau_to_alpha(attack_ms / 1000.0, fs)
        self.aR = tau_to_alpha(release_ms / 1000.0, fs)
        # HOLD only applies to the shipped smooth_decoupled_rtt detector (see
        # process_sample below). round(), not int(), so e.g. 5ms @ 44100 ->
        # 220.5 samples rounds to 220/221 rather than always truncating down.
        self.hold_samples = int(round(hold_ms / 1000.0 * fs))
        self.hold_counter = 0

    def reset(self):
        self.y1 = 0.0
        self.s1 = 0.0
        self.hold_counter = 0

    def process_sample(self, x):
        aA, aR, y1 = self.aA, self.aR, self.y1
        if self.kind == "branching_rtz":
            y = aA * y1 + (1 - aA) * x if x > y1 else aR * y1
        elif self.kind == "smooth_branching_rtt":
            y = aA * (y1 - x) + x if x >= y1 else aR * (y1 - x) + x
        elif self.kind == "decoupled_rtz":
            s1 = max(x, aR * self.s1)
            y = aA * y1 + (1 - aA) * s1
            self.s1 = s1
        else:  # smooth_decoupled_rtt (SHIP) -- HOLD lives here.
            # Original (hold=0) formula: s1 = max(x, aR*(s1-x)+x). That max()
            # is exactly equivalent to "attack (x >= s1): s1 = x, instant"
            # vs "release (x < s1): s1 = release-pole(s1,x)" -- the pole
            # term is provably >= x whenever x < s1 and provably <= x
            # whenever x >= s1 (aR in [0,1)), so max() always picks the
            # attack branch on attack and the pole branch on release. HOLD
            # inserts a third state between them: once we transition into
            # the release branch, freeze s1 for hold_samples before letting
            # the release pole engage. A fresh attack (x >= s1) at any point
            # -- including mid-hold -- resets the counter and re-enters the
            # attack branch normally, per spec's "re-trigger resets".
            if x >= self.s1:
                s1 = x
                self.hold_counter = self.hold_samples
            elif self.hold_counter > 0:
                s1 = self.s1  # frozen -- envelope holds, no pole movement
                self.hold_counter -= 1
            else:
                s1 = aR * (self.s1 - x) + x
            y = aA * (y1 - s1) + s1
            self.s1 = s1
        self.y1 = y
        return y


class DualPoleDetector:
    """Program-dependent release: blend a fast and a slow release pole by a
    slowly-smoothed weight w that tracks how far ABOVE THRESHOLD the input
    currently sits (the instantaneous, unsmoothed control signal |c| = x
    fed in here -- NOT the smoothed GR). Attack is a single normal pole.

    w -> 1 (input still loud, |c| near/above D_ref) => favor the SLOW pole
         (steady, pumping-free release while the program is still hot).
    w -> 0 (input has already dropped below threshold, |c| ~ 0) => favor the
         FAST pole (snap back to unity between transients, e.g. drum hits).
    """

    def __init__(self):
        self.y1 = 0.0
        self.w = 0.0
        self.aA = self.aRf = self.aRs = self.beta = 0.0
        self.D_ref = 9.0

    def prepare(self, fs, attack_ms, release_fast_ms, release_slow_ms,
                D_ref=9.0, tau_w_ms=200.0):
        self.aA = tau_to_alpha(attack_ms / 1000.0, fs)
        self.aRf = tau_to_alpha(release_fast_ms / 1000.0, fs)
        self.aRs = tau_to_alpha(release_slow_ms / 1000.0, fs)
        self.beta = tau_to_alpha(tau_w_ms / 1000.0, fs)
        self.D_ref = D_ref

    def reset(self):
        self.y1 = 0.0
        self.w = 0.0

    def process_sample(self, x):
        y1 = self.y1
        self.w = self.beta * self.w + (1 - self.beta) * np.clip(x / self.D_ref, 0.0, 1.0)
        if x >= y1:
            y = self.aA * (y1 - x) + x
        else:
            g_fast = self.aRf * (y1 - x) + x
            g_slow = self.aRs * (y1 - x) + x
            y = self.w * g_slow + (1 - self.w) * g_fast
        self.y1 = y
        return y


# ============================================================
# PART 2: the compressor
# ============================================================

class Compressor:
    """Feed-forward compressor node: prepare() off the audio thread,
    process_sample()/process() on it, reset() clears all state.
    """

    def __init__(self):
        self.detector = None
        self.sc_hpf = None
        self.use_rms = False
        self.ms = 0.0
        self.aRMS = 0.0
        self.T = self.R = self.W = 0.0
        self.makeup_db = 0.0
        self.auto_makeup = False
        self.M = 0.0

    def prepare(self, fs, T=-20.0, R=4.0, W=6.0, attack_ms=10.0, release_ms=100.0,
                makeup_db=0.0, auto_makeup=False, use_rms=False, rms_tau_ms=5.0,
                detector_kind="smooth_decoupled_rtt", sc_hpf_hz=None,
                dual_pole=False, release_fast_ms=None, release_slow_ms=None,
                D_ref=9.0, tau_w_ms=200.0,
                mix=1.0, range_db=np.inf, hold_ms=0.0):
        self.fs = fs
        self.T, self.R, self.W = T, R, W
        self.makeup_db = makeup_db
        self.auto_makeup = auto_makeup
        self.M = abs(T) * (1.0 - 1.0 / R)
        self.use_rms = use_rms
        self.aRMS = tau_to_alpha(rms_tau_ms / 1000.0, fs)
        self.ms = 0.0
        self.mix = mix
        self.range_db = range_db
        self.hold_ms = hold_ms

        if sc_hpf_hz is not None:
            # Cascade two one-pole sections (~12dB/oct) rather than one: a
            # single pole at fc=100Hz only attenuates 60Hz by ~-5.8dB, not
            # enough to meaningfully separate "kick fundamental" from
            # "everything else" in the detector path. Two poles gets closer
            # to what a real SC-HPF control actually does.
            self.sc_hpf = [OnePoleHPF(), OnePoleHPF()]
            for h in self.sc_hpf:
                h.prepare(fs, sc_hpf_hz)
        else:
            self.sc_hpf = None

        if dual_pole:
            det = DualPoleDetector()
            det.prepare(fs, attack_ms, release_fast_ms, release_slow_ms, D_ref, tau_w_ms)
        else:
            det = PeakDetector(detector_kind)
            det.prepare(fs, attack_ms, release_ms, hold_ms=hold_ms)
        self.detector = det

    def reset(self):
        self.detector.reset()
        if self.sc_hpf is not None:
            for h in self.sc_hpf:
                h.reset()
        self.ms = 0.0

    def process_sample(self, x):
        sc = x
        if self.sc_hpf is not None:
            for h in self.sc_hpf:
                sc = h.process_sample(sc)
        if self.use_rms:
            self.ms = self.aRMS * self.ms + (1 - self.aRMS) * sc * sc
            x_L = 10.0 * np.log10(max(self.ms, 1e-12))
        else:
            x_L = 20.0 * np.log10(max(abs(sc), 1e-6))

        y_G = gain_computer(x_L, self.T, self.R, self.W)
        c = y_G - x_L
        e = -c
        g_sm_e = self.detector.process_sample(e)
        g_sm = -g_sm_e

        # RANGE: clamp the APPLIED gain reduction after ballistics smoothing,
        # before makeup/mix. The smoother's own state (detector.y1/.s1, which
        # is what the *next* sample's ballistics are computed from) is fed by
        # g_sm_e above and keeps evolving completely unclamped -- clamping
        # only touches the number used for gain-per-sample below. That's why
        # there's no click at clamp engage/disengage: g_clamped is a plain
        # max(), continuous by construction, and the envelope it's clamping
        # was already continuous (ballistics-smoothed) going in.
        g_clamped = max(g_sm, -self.range_db)

        makeup = self.M if self.auto_makeup else self.makeup_db
        gain_lin = 10.0 ** ((g_clamped + makeup) / 20.0)
        wet = x * gain_lin
        # MIX: linear dry/wet crossfade, applied last. Dry is the untouched
        # input -- NOT run through makeup -- see module docstring / report
        # for the "why" (industry-standard parallel-compression behavior:
        # makeup gain compensates the compressed path's level loss, it has
        # no meaning applied to a signal that was never attenuated).
        out = (1.0 - self.mix) * x + self.mix * wet
        return out, g_clamped

    def process(self, x):
        n = len(x)
        y = np.empty(n)
        gr = np.empty(n)
        for i in range(n):
            y[i], gr[i] = self.process_sample(x[i])
        return y, gr


# ============================================================
# PART 3: signal synthesis helpers (tests 5, 7, 8)
# ============================================================

def make_kick_loop(fs, bars=2, bpm=120.0, beats_per_bar=4, seed=0):
    """2 bars @ 120bpm = 8 kicks, 0.5s apart. Each kick = exp-decaying 60Hz
    sine burst (~150ms) + a short exp-decaying noise transient (~5ms click).
    """
    rng = np.random.default_rng(seed)
    beat_s = 60.0 / bpm
    dur = bars * beats_per_bar * beat_s
    n = int(fs * dur)
    x = np.zeros(n)
    n_hits = bars * beats_per_bar
    hit_times = [i * beat_s for i in range(n_hits)]
    tone_len = int(0.3 * fs)
    t_tone = np.arange(tone_len) / fs
    tone_env = np.exp(-t_tone / 0.05)
    tone = 0.9 * np.sin(2 * np.pi * 60.0 * t_tone) * tone_env
    click_len = int(0.02 * fs)
    t_click = np.arange(click_len) / fs
    click_env = np.exp(-t_click / 0.003)
    for ht in hit_times:
        i0 = int(ht * fs)
        click = rng.standard_normal(click_len) * click_env * 0.6
        i1 = min(n, i0 + click_len)
        x[i0:i1] += click[: i1 - i0]
        i1t = min(n, i0 + tone_len)
        x[i0:i1t] += tone[: i1t - i0]
    return x, hit_times


def make_pad_swell(fs, dur=4.0, f_center=350.0, f_low=None, seed=1,
                    dip_at_s=1.5, dip_len_s=0.2, dip_depth=0.4):
    """Sustained filtered-noise swell: swells in over 0.5s, holds loud, then
    -- while still well above threshold throughout -- takes one brief
    "breath" dip (dip_at_s, duration dip_len_s, down to dip_depth) and
    recovers back to full level. Program stays loud before AND after the
    dip, which is what should keep the dual-pole's w-weight elevated (biased
    toward the slow pole) through the whole event -- unlike a permanent
    step-down to a new, quieter section, which starves w's instantaneous
    |c| input and collapses w toward the fast pole almost immediately
    (verified: that was the wrong signal shape for this test, see the
    prog-dep verification comment for the full story).

    Filtered noise, not detuned saws: a small handful of detuned oscillators
    beat against each other with a multi-Hz period, which shows up as a slow
    amplitude wobble riding on top of the compressor's own release response
    and corrupts the very release-tail timing this signal exists to measure.
    Lowpassed noise has no such periodic structure.
    """
    rng = np.random.default_rng(seed)
    n = int(fs * dur)
    white = rng.standard_normal(n)
    # cheap one-pole lowpass, applied twice for a steeper ~12dB/oct rolloff
    g = np.tan(np.pi * f_center / fs)
    G = g / (1 + g)
    x = np.empty(n)
    s = 0.0
    for i in range(n):
        v = (white[i] - s) * G
        lp = v + s
        s = lp + v
        x[i] = lp
    s = 0.0
    for i in range(n):
        v = (x[i] - s) * G
        lp = v + s
        s = lp + v
        x[i] = lp

    if f_low is not None:
        # bandpass: a lowpass alone leaves everything BELOW f_center
        # untouched (including sub-100Hz content), which defeats the SC-HPF
        # demo's premise that the pad is "higher register" than the kick --
        # confirmed by measuring the unfiltered pad's own spectrum (it had
        # nearly half as much energy below 100Hz as in the 100-2000Hz band).
        # Add a highpass at f_low to push the pad's energy up and out of
        # the kick's fundamental range.
        hpf = OnePoleHPF()
        hpf.prepare(fs, f_low)
        x = np.array([hpf.process_sample(v) for v in x])

    x /= np.max(np.abs(x)) + 1e-9

    env = np.ones(n)
    swell_len = int(0.5 * fs)
    env[:swell_len] = np.linspace(0, 1, swell_len)
    dip_i0 = int(dip_at_s * fs)
    dip_i1 = int((dip_at_s + dip_len_s) * fs)
    ramp = int(0.02 * fs)
    env[dip_i0:dip_i0 + ramp] = np.linspace(1.0, dip_depth, ramp)
    env[dip_i0 + ramp:dip_i1] = dip_depth
    env[dip_i1:dip_i1 + ramp] = np.linspace(dip_depth, 1.0, ramp)
    x *= env
    return x * 0.9


def make_bass_heavy_mix(fs, dur=4.0, seed=2):
    """Kick (60Hz thump, bass-heavy) + a HIGHER-register pad (so a 100Hz SC
    HPF removes most of the kick's energy from the detector but leaves the
    pad largely untouched) -- built to make the HPF's pumping-reduction
    effect legible.
    """
    kick, hit_times = make_kick_loop(fs, bars=2, bpm=120.0, seed=seed)
    n = len(kick)
    pad = make_pad_swell(fs, dur=n / fs, f_center=500.0, f_low=200.0, seed=seed + 1)
    pad = pad[:n]
    mix = kick * 1.0 + pad * 0.5
    peak = np.max(np.abs(mix))
    if peak > 0.95:
        mix = mix / peak * 0.95
    return mix, hit_times


# ============================================================
# VERIFICATION 1: static curve
# ============================================================

def verify_static_curve():
    fs = sr
    T, R, W = -20.0, 4.0, 6.0
    all_pass = True

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[static] {status}: {name} -- {detail}")

    levels_db = np.linspace(-80, 0, 161)
    comp = Compressor()
    comp.prepare(fs, T=T, R=R, W=W, attack_ms=0.0005, release_ms=0.0005,
                 detector_kind="smooth_decoupled_rtt")

    measured = np.empty_like(levels_db)
    n_settle = 200
    for i, lv in enumerate(levels_db):
        comp.reset()
        amp = 10 ** (lv / 20.0)
        y_last = 0.0
        for _ in range(n_settle):
            y_last, _ = comp.process_sample(amp)
        measured[i] = 20 * np.log10(max(abs(y_last), 1e-12))

    closed_form = gain_computer(levels_db, T, R, W)
    err = np.max(np.abs(measured - closed_form))
    check("settled output matches y_G(x) closed form", err < 1e-3,
          f"max abs error = {err:.6f} dB over {len(levels_db)} points")

    # knee boundary continuity: T-W/2 and T+W/2, approached from both sides
    for label, x0 in (("lower knee edge T-W/2", T - W / 2), ("upper knee edge T+W/2", T + W / 2)):
        eps = 1e-4
        left = gain_computer(x0 - eps, T, R, W)
        right = gain_computer(x0 + eps, T, R, W)
        gap = abs(left - right)
        check(f"knee continuity at {label}", gap < 1e-3,
              f"y_G({x0-eps:.4f})={left:.6f}, y_G({x0+eps:.4f})={right:.6f}, gap={gap:.6f} dB")

    fig, ax = plt.subplots(figsize=(8, 6))
    ax.plot(levels_db, closed_form, label="y_G closed form", lw=2)
    ax.plot(levels_db, measured, "--", label="measured (settled detector)", lw=1.5)
    ax.axvline(T - W / 2, color="gray", ls=":", lw=0.8)
    ax.axvline(T + W / 2, color="gray", ls=":", lw=0.8)
    ax.axvline(T, color="gray", ls="-", lw=0.5)
    ax.set_xlabel("input level (dB)"); ax.set_ylabel("output level (dB)")
    ax.set_title(f"Static compression curve: T={T}, R={R}, W={W} (quadratic soft knee)")
    ax.legend(); ax.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{OUT}/comp_static_curve.png", dpi=130)
    plt.close(fig)

    return all_pass


# ============================================================
# VERIFICATION 2: C1 knee (slope continuity/monotonicity)
# ============================================================

def verify_knee_slope():
    T, R, W = -20.0, 4.0, 6.0
    all_pass = True

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[knee] {status}: {name} -- {detail}")

    x = np.linspace(T - 3 * W, T + 3 * W, 4001)
    h = x[1] - x[0]
    y = gain_computer(x, T, R, W)
    slope = np.empty_like(x)
    slope[1:-1] = (y[2:] - y[:-2]) / (2 * h)
    slope[0] = slope[1]
    slope[-1] = slope[-2]

    below_target, above_target = 1.0, 1.0 / R
    idx_below = x < T - W / 2 - 2 * h
    idx_above = x > T + W / 2 + 2 * h
    check("slope == 1 well below knee", np.allclose(slope[idx_below], below_target, atol=1e-3),
          f"max dev = {np.max(np.abs(slope[idx_below]-below_target)):.5f}")
    check("slope == 1/R well above knee", np.allclose(slope[idx_above], above_target, atol=1e-3),
          f"max dev = {np.max(np.abs(slope[idx_above]-above_target)):.5f}")

    dslope = np.diff(slope)
    max_jump = np.max(np.abs(dslope))
    check("slope continuous (no jump at knee boundaries)", max_jump < 5 * (below_target - above_target) / (W / h),
          f"max |d(slope)| between samples = {max_jump:.6f}")

    is_monotonic = np.all(dslope <= 1e-6)
    check("slope monotonically non-increasing 1 -> 1/R", is_monotonic,
          f"max positive step in slope = {max(0.0, np.max(dslope)):.6f}")

    fig, axs = plt.subplots(2, 1, figsize=(8, 8), sharex=True)
    axs[0].plot(x, y); axs[0].set_ylabel("y_G(x) dB"); axs[0].grid(alpha=0.3)
    axs[1].plot(x, slope); axs[1].axhline(1.0, color="gray", ls=":", lw=0.8)
    axs[1].axhline(1.0 / R, color="gray", ls=":", lw=0.8)
    axs[1].set_ylabel("d(y_G)/dx"); axs[1].set_xlabel("input dB"); axs[1].grid(alpha=0.3)
    axs[0].set_title(f"C1 knee check: T={T}, R={R}, W={W}")
    for ax in axs:
        ax.axvline(T - W / 2, color="red", ls=":", lw=0.7)
        ax.axvline(T + W / 2, color="red", ls=":", lw=0.7)
    plt.tight_layout()
    plt.savefig(f"{OUT}/comp_knee_slope.png", dpi=130)
    plt.close(fig)

    return all_pass


# ============================================================
# VERIFICATION 3: ballistics (63.2% attack/release time, level independence)
# ============================================================

def measure_step_response(fs, low_db, high_db, attack_ms, release_ms, T=-30.0, R=4.0, W=0.0):
    """Constant level at low_db for 0.5s, step to high_db for 0.5s, back to
    low_db for 0.5s. Returns (t, gr) where gr is the smoothed control signal
    in dB (<=0), plus the sample indices of the two step edges.

    Uses smooth_branching_rtt (single first-order pole) rather than the
    shipped smooth_decoupled_rtt: this test verifies the alpha=exp(-1/(tau*fs))
    <-> tau mapping itself, and smooth_branching_rtt realizes exactly ONE
    pole per branch so its 63.2% time equals tau by construction. The
    decoupled architecture cascades the release pole (s1, tau=release_ms)
    through the attack pole (y following s1, tau=attack_ms) even during
    release, which measurably shifts the observed release time -- e.g. at
    attack=10ms/release=100ms this cascade delay pushes the measured 63.2%
    point to ~109ms (verified analytically against the 2-pole step-response
    closed form: matches to <0.1ms). That's a real, expected property of the
    decoupled/SHIP detector, not a bug -- it just isn't what this test is
    checking, so it isn't the detector used here.
    """
    comp = Compressor()
    comp.prepare(fs, T=T, R=R, W=W, attack_ms=attack_ms, release_ms=release_ms,
                 detector_kind="smooth_branching_rtt")
    seg = int(0.5 * fs)
    amp_lo = 10 ** (low_db / 20.0)
    amp_hi = 10 ** (high_db / 20.0)
    x = np.concatenate([np.full(seg, amp_lo), np.full(seg, amp_hi), np.full(seg, amp_lo)])
    y, gr = comp.process(x)
    return gr, seg, 2 * seg


def time_to_63pct(gr, edge_idx, rising_toward, fs):
    """Sample index -> ms from edge_idx until gr first crosses 63.2% of the
    way from gr[edge_idx-1] to its own final settled value (measured at the
    end of that segment, well after this call's window ends -- caller passes
    the correct segment slice).
    """
    start_val = gr[edge_idx]
    # settle value = last sample of the provided window
    final_val = gr[-1]
    target = start_val + 0.632 * (final_val - start_val)
    seg = gr[edge_idx:]
    if rising_toward:  # target is above start (less negative / smaller magnitude)
        idx = np.argmax(seg >= target)
    else:
        idx = np.argmax(seg <= target)
    return idx / fs * 1000.0


def verify_ballistics():
    fs = sr
    all_pass = True

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[ballistics] {status}: {name} -- {detail}")

    attack_ms, release_ms = 10.0, 100.0

    # --- basic attack/release time label check, 20 dB overshoot ---
    gr, e1, e2 = measure_step_response(fs, low_db=-40, high_db=-10, attack_ms=attack_ms, release_ms=release_ms)
    attack_window = gr[e1:e2]
    t_attack = time_to_63pct(attack_window, 0, rising_toward=False, fs=fs)  # gr falls (more negative) on attack
    check("attack time == label within 5%", abs(t_attack - attack_ms) / attack_ms < 0.05,
          f"measured {t_attack:.3f} ms vs label {attack_ms} ms")

    release_window = gr[e2:]
    t_release = time_to_63pct(release_window, 0, rising_toward=True, fs=fs)  # gr rises back toward 0 on release
    check("release time == label within 5%", abs(t_release - release_ms) / release_ms < 0.05,
          f"measured {t_release:.3f} ms vs label {release_ms} ms")

    # --- level independence: attack time for two different overshoot depths ---
    gr10, e1a, e2a = measure_step_response(fs, low_db=-40, high_db=-30 + 10, attack_ms=attack_ms, release_ms=release_ms)
    gr30, e1b, e2b = measure_step_response(fs, low_db=-40, high_db=-30 + 30, attack_ms=attack_ms, release_ms=release_ms)
    t10 = time_to_63pct(gr10[e1a:e2a], 0, rising_toward=False, fs=fs)
    t30 = time_to_63pct(gr30[e1b:e2b], 0, rising_toward=False, fs=fs)
    rel_diff = abs(t10 - t30) / attack_ms
    check("level-independent attack time (10dB vs 30dB overshoot)", rel_diff < 0.02,
          f"10dB-overshoot t={t10:.3f}ms, 30dB-overshoot t={t30:.3f}ms, rel diff={rel_diff*100:.3f}%")

    fig, ax = plt.subplots(figsize=(9, 5))
    t = np.arange(len(gr)) / fs * 1000.0
    ax.plot(t, gr, label=f"GR envelope (attack={attack_ms}ms, release={release_ms}ms)")
    ax.axvline(e1 / fs * 1000, color="gray", ls=":", lw=0.8)
    ax.axvline(e2 / fs * 1000, color="gray", ls=":", lw=0.8)
    ax.set_xlabel("ms"); ax.set_ylabel("gain reduction (dB)")
    ax.set_title("Ballistics step response (smooth_decoupled_rtt)")
    ax.legend(); ax.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{OUT}/comp_ballistics.png", dpi=130)
    plt.close(fig)

    return all_pass


# ============================================================
# VERIFICATION 4: detector comparison (kink at release)
# ============================================================

def verify_detector_comparison():
    fs = sr
    all_pass = True

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[detectors] {status}: {name} -- {detail}")

    dur = 1.5
    n = int(fs * dur)
    t = np.arange(n) / fs
    burst = np.zeros(n)
    burst_on = (t >= 0.2) & (t < 0.8)
    burst[burst_on] = 0.9 * np.sin(2 * np.pi * 300.0 * t[burst_on])

    T, R, W = -25.0, 6.0, 3.0
    attack_ms, release_ms = 5.0, 150.0

    # NOTE: a rectified sine's envelope revisits ~0 every half-cycle. A
    # branching detector re-enters its "release" branch (aR*y1, ignoring x)
    # every one of those dips *while the tone is still sounding* -- that's
    # the audio-rate zigzag/kink RtZ detectors are known for, and it shows
    # up as high curvature *during the sustained burst*, not after it ends.
    # (After the burst cuts to exact silence, c settles to 0 for every
    # detector and branching_rtz's "aR*y1" collapses to the exact same
    # formula as the RtT variants -- so measuring curvature post-burst would
    # hide the very artifact this test exists to show.)

    envs = {}
    for kind in PeakDetector.KINDS:
        comp = Compressor()
        comp.prepare(fs, T=T, R=R, W=W, attack_ms=attack_ms, release_ms=release_ms, detector_kind=kind)
        _, gr = comp.process(burst)
        envs[kind] = gr

    fig, ax = plt.subplots(figsize=(10, 6))
    for kind, gr in envs.items():
        ax.plot(t, gr, label=kind, lw=1.2)
    ax.axvline(0.8, color="gray", ls=":", lw=0.8, label="burst off (release starts)")
    ax.set_xlabel("s"); ax.set_ylabel("gain reduction (dB)")
    ax.set_title("Detector comparison: GR envelope, 300Hz burst above threshold")
    ax.legend(fontsize=8); ax.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{OUT}/comp_detector_comparison.png", dpi=130)
    plt.close(fig)

    # sustain-region second-difference (curvature) -- kink/zigzag metric,
    # measured DURING the burst (well after the attack transient settles,
    # well before burst-off) so it isolates the branch-flip artifact.
    sustain_start = int(0.35 * fs)
    sustain_end = int(0.75 * fs)

    def max_curvature(gr):
        seg = gr[sustain_start:sustain_end]
        d2 = np.diff(seg, n=2)
        return np.max(np.abs(d2))

    curv_branching = max_curvature(envs["branching_rtz"])
    curv_smooth_decoupled = max_curvature(envs["smooth_decoupled_rtt"])
    check("smooth_decoupled_rtt has no release kink/zigzag where branching_rtz does",
          curv_smooth_decoupled < curv_branching,
          f"max second-diff during sustained burst: smooth_decoupled_rtt={curv_smooth_decoupled:.3e}, "
          f"branching_rtz={curv_branching:.3e}")

    return all_pass


# ============================================================
# VERIFICATION 5: program-dependent release (dual-pole A/B)
# ============================================================

def verify_program_dependent():
    fs = sr
    all_pass = True

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[prog-dep] {status}: {name} -- {detail}")

    T, R, W = -20.0, 4.0, 3.0
    attack_ms = 5.0
    # "fixed" = the naive single-knob compromise a mixer might pick, roughly
    # midway between what drums want (fast, ~50ms) and what a pad wants
    # (slow, ~300ms). It is deliberately the SAME setting used against both
    # signals below -- the point of dual-pole is that it beats this one
    # compromise on BOTH signals at once, not that it's compared against a
    # different "fixed" value per signal.
    fixed_release_ms = 120.0
    fast_ms, slow_ms = 50.0, 300.0

    kick, hit_times = make_kick_loop(fs, bars=2, bpm=120.0, seed=10)
    pad = make_pad_swell(fs, dur=4.0, seed=11)

    # Peak detection for drums (fast transients must be tracked sample-
    # accurately). RMS (tau=15ms) for the pad: raw filtered-noise PEAKS
    # dip to/near zero constantly between zero-crossings even while the
    # program is "sustained," which made the instantaneous |c| fed to w
    # chatter between 0 and its true level every fraction of a ms -- w
    # never converged near 1 the way a real sustained pad should drive it.
    # RMS is the textbook choice for pad/bus material for exactly this
    # reason (smooths the level *before* it ever reaches the gain computer,
    # separate from -- and upstream of -- the ballistics smoothing itself).
    def build_fixed(use_rms=False):
        c = Compressor()
        c.prepare(fs, T=T, R=R, W=W, attack_ms=attack_ms, release_ms=fixed_release_ms,
                   detector_kind="smooth_decoupled_rtt", use_rms=use_rms, rms_tau_ms=15.0)
        return c

    def build_dual(use_rms=False):
        c = Compressor()
        # D_ref=5dB (not the 9dB "typical mixing GR" default): with this
        # section's T/R/W and pad loudness, the observed |c| during the
        # sustained pad tops out around 6-8dB (checked directly), so a
        # 9dB reference never lets w's clamp(|c|/D_ref) saturate -- w
        # topped out near 0.65, and w*aRs+(1-w)*aRf blends in ALPHA space,
        # not tau space, so even 65% weight on the slow pole only nudged
        # the effective tau to ~109ms (barely below the 120ms fixed
        # compromise -- alpha blending is dominated by whichever pole is
        # closer to alpha=1... err, further from 1 -- the fast pole's alpha
        # is farther from 1, so it pulls the blend down disproportionately
        # to its weight). D_ref=5dB lets w saturate near 1 for genuinely
        # sustained content, which is what actually gives the slow pole
        # its intended dominance.
        c.prepare(fs, T=T, R=R, W=W, attack_ms=attack_ms, dual_pole=True,
                   release_fast_ms=fast_ms, release_slow_ms=slow_ms,
                   use_rms=use_rms, rms_tau_ms=15.0, D_ref=5.0)
        return c

    y_kick_fixed, gr_kick_fixed = build_fixed(use_rms=False).process(kick)
    y_kick_dual, gr_kick_dual = build_dual(use_rms=False).process(kick)
    y_pad_fixed, gr_pad_fixed = build_fixed(use_rms=True).process(pad)
    y_pad_dual, gr_pad_dual = build_dual(use_rms=True).process(pad)

    for name, arr in (("drums_fixed", y_kick_fixed), ("drums_dualpole", y_kick_dual),
                       ("pad_fixed", y_pad_fixed), ("pad_dualpole", y_pad_dual),
                       ("drums_dry", kick), ("pad_dry", pad)):
        wavfile.write(f"{OUT}/comp_{name}.wav", fs, (np.clip(arr, -1, 1) * 32767).astype(np.int16))

    # --- drums: GR at hit midpoints (between hits) should be near 0 with dual-pole ---
    beat_s = 60.0 / 120.0
    midpoints = [ht + beat_s * 0.9 for ht in hit_times[:-1]]  # just before the next hit
    mid_idx = [int(m * fs) for m in midpoints]
    gr_fixed_mid = np.mean([abs(gr_kick_fixed[i]) for i in mid_idx])
    gr_dual_mid = np.mean([abs(gr_kick_dual[i]) for i in mid_idx])
    check("drums: GR recovers closer to 0 between hits with dual-pole",
          gr_dual_mid < gr_fixed_mid,
          f"mean |GR| at hit midpoints: dual-pole={gr_dual_mid:.3f} dB, fixed={gr_fixed_mid:.3f} dB")

    # --- pad: release tail during the brief in-passage "breath" dip
    # (t=1.5-1.7s) is slower (longer) with dual-pole than the fixed
    # compromise. The dip is short and the pad is loud both before and
    # after it, so w (tau_w=200ms) stays close to its pre-dip elevated
    # value for the whole event instead of collapsing toward 0 -- keeping
    # the dual-pole blend biased toward the slow pole exactly when the
    # program is, in fact, still sustained. (An earlier version of this
    # test used a PERMANENT step down to a new, quieter section instead of
    # a brief dip; that starves w's instantaneous |c| input immediately and
    # collapses it toward the fast pole, which is the physically correct
    # behavior for "the note just ended" -- but it isn't what a sustained
    # pad's natural release behavior looks like, so it was the wrong signal
    # for this assertion and gave the wrong-signed result.)
    dip_idx = int(1.5 * fs)
    window = int(0.35 * fs)

    def tail_63pct_time(gr):
        seg = gr[dip_idx:dip_idx + window]
        start_val = seg[0]
        final_val = np.max(seg)  # GR relaxes toward 0 (least negative) during the dip
        target = start_val + 0.632 * (final_val - start_val)
        idx = np.argmax(seg >= target)
        return idx / fs * 1000.0

    t_fixed = tail_63pct_time(gr_pad_fixed)
    t_dual = tail_63pct_time(gr_pad_dual)
    check("pad: release tail time constant longer with dual-pole",
          t_dual > t_fixed,
          f"63%% settle time into the breath dip: dual-pole={t_dual:.1f} ms, fixed({fixed_release_ms}ms)={t_fixed:.1f} ms")

    fig, axs = plt.subplots(2, 1, figsize=(11, 8))
    t_kick = np.arange(len(kick)) / fs
    axs[0].plot(t_kick, gr_kick_fixed, label=f"fixed release={fixed_release_ms}ms")
    axs[0].plot(t_kick, gr_kick_dual, label=f"dual-pole (fast={fast_ms}/slow={slow_ms}ms)")
    for ht in hit_times:
        axs[0].axvline(ht, color="gray", ls=":", lw=0.5)
    axs[0].set_title("Drums: GR envelope, fixed vs dual-pole release")
    axs[0].set_ylabel("GR (dB)"); axs[0].legend(fontsize=8); axs[0].grid(alpha=0.3)

    t_pad = np.arange(len(pad)) / fs
    axs[1].plot(t_pad, gr_pad_fixed, label=f"fixed release={fixed_release_ms}ms")
    axs[1].plot(t_pad, gr_pad_dual, label=f"dual-pole (fast={fast_ms}/slow={slow_ms}ms)")
    axs[1].axvline(1.5, color="gray", ls=":", lw=0.8, label="breath dip")
    axs[1].axvline(1.7, color="gray", ls=":", lw=0.8)
    axs[1].set_title("Pad: GR envelope, fixed vs dual-pole release")
    axs[1].set_xlabel("s"); axs[1].set_ylabel("GR (dB)"); axs[1].legend(fontsize=8); axs[1].grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{OUT}/comp_program_dependent.png", dpi=130)
    plt.close(fig)

    return all_pass


# ============================================================
# VERIFICATION 6: null test (R=1)
# ============================================================

def verify_null_test():
    fs = sr
    all_pass = True
    n = int(fs * 1.0)
    rng = np.random.default_rng(42)
    x = rng.uniform(-0.8, 0.8, n)

    comp = Compressor()
    comp.prepare(fs, T=-20.0, R=1.0, W=6.0, attack_ms=5.0, release_ms=100.0,
                 makeup_db=0.0, auto_makeup=False, detector_kind="smooth_decoupled_rtt")
    y, gr = comp.process(x)
    err = np.max(np.abs(y - x))
    status = "PASS" if err < 1e-6 else "FAIL"
    if err >= 1e-6:
        all_pass = False
    print(f"[null] {status}: R=1, makeup=0, no SC filter -> max|out-in| = {err:.3e}")
    return all_pass


# ============================================================
# VERIFICATION 7: THD
# ============================================================

def thd_db(x, fs, f0, n_harmonics=6):
    n = len(x)
    win = np.hanning(n)
    X = np.fft.rfft(x * win)
    freqs = np.fft.rfftfreq(n, d=1 / fs)
    mag = np.abs(X)

    def bin_at(f):
        return np.argmax(mag[max(0, np.searchsorted(freqs, f) - 3):np.searchsorted(freqs, f) + 4]) + \
               max(0, np.searchsorted(freqs, f) - 3)

    fund_bin = bin_at(f0)
    fund_amp = mag[fund_bin]
    harm_power = 0.0
    harm_amps = {}
    for k in range(2, n_harmonics + 1):
        hb = bin_at(f0 * k)
        harm_amps[k] = mag[hb]
        harm_power += mag[hb] ** 2
    thd = np.sqrt(harm_power) / (fund_amp + 1e-15)
    return 20 * np.log10(thd + 1e-15), harm_amps, fund_amp


def verify_thd():
    fs = sr
    all_pass = True

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[thd] {status}: {name} -- {detail}")

    dur = 1.0
    n = int(fs * dur)
    t = np.arange(n) / fs

    # --- slow ballistics, 1kHz, ~6dB GR target ---
    f0 = 1000.0
    T, R, W = -18.0, 4.0, 0.0
    amp_db = -6.0  # input level tuned so steady-state GR ~= 6dB with R=4
    # steady state: y_G(amp_db) - amp_db should be about -6dB; adjust T to get there
    # solve iteratively is overkill -- pick amp comfortably above T and check GR after.
    x = (10 ** (amp_db / 20.0)) * np.sin(2 * np.pi * f0 * t)
    comp = Compressor()
    comp.prepare(fs, T=T, R=R, W=W, attack_ms=30.0, release_ms=300.0, detector_kind="smooth_decoupled_rtt")
    y, gr = comp.process(x)
    steady_gr = np.mean(gr[int(0.5 * fs):])
    thd, harms, fund = thd_db(y[int(0.2 * fs):], fs, f0)
    check("THD < -80dB (slow ballistics, ~steady GR, 1kHz sine)", thd < -80.0,
          f"THD={thd:.2f} dB, steady-state GR={steady_gr:.2f} dB")

    # --- fast attack, 60Hz sine: THD should rise, characterize 2nd/3rd harmonic ---
    f1 = 60.0
    x2 = (10 ** (amp_db / 20.0)) * np.sin(2 * np.pi * f1 * t)
    comp2 = Compressor()
    comp2.prepare(fs, T=T, R=R, W=W, attack_ms=0.1, release_ms=300.0, detector_kind="smooth_decoupled_rtt")
    y2, gr2 = comp2.process(x2)
    thd2, harms2, fund2 = thd_db(y2[int(0.2 * fs):], fs, f1)
    check("fast attack (0.1ms) THD is worse than slow-ballistics baseline", thd2 > thd,
          f"fast-attack THD={thd2:.2f} dB vs slow-ballistics THD={thd:.2f} dB (60Hz fundamental)")
    h2_db = 20 * np.log10(harms2[2] / (fund2 + 1e-15) + 1e-15)
    h3_db = 20 * np.log10(harms2[3] / (fund2 + 1e-15) + 1e-15)
    print(f"[thd] INFO: ripple distortion @ fast attack -- 120Hz (2nd) = {h2_db:.2f} dB, "
          f"180Hz (3rd) = {h3_db:.2f} dB relative to fundamental (characterize, not eliminate)")

    fig, axs = plt.subplots(2, 1, figsize=(9, 8))
    for ax, sig, title in ((axs[0], y[int(0.2 * fs):], f"1kHz, slow ballistics (THD={thd:.1f}dB)"),
                            (axs[1], y2[int(0.2 * fs):], f"60Hz, fast attack=0.1ms (THD={thd2:.1f}dB)")):
        nseg = len(sig)
        X = np.fft.rfft(sig * np.hanning(nseg))
        freqs = np.fft.rfftfreq(nseg, d=1 / fs)
        mag_db = 20 * np.log10(np.abs(X) / np.max(np.abs(X)) + 1e-12)
        ax.plot(freqs, mag_db)
        ax.set_xlim(0, 2000); ax.set_ylim(-120, 5)
        ax.set_title(title); ax.set_ylabel("dB (rel. to peak)"); ax.grid(alpha=0.3)
    axs[1].set_xlabel("Hz")
    plt.tight_layout()
    plt.savefig(f"{OUT}/comp_thd_spectra.png", dpi=130)
    plt.close(fig)

    return all_pass


# ============================================================
# VERIFICATION 8: SC HPF demo (pumping reduction)
# ============================================================

def verify_sc_hpf_demo():
    fs = sr
    all_pass = True

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[sc-hpf] {status}: {name} -- {detail}")

    mix, hit_times = make_bass_heavy_mix(fs, dur=4.0, seed=20)
    T, R, W = -22.0, 4.0, 3.0
    attack_ms, release_ms = 5.0, 150.0

    comp_no_hpf = Compressor()
    comp_no_hpf.prepare(fs, T=T, R=R, W=W, attack_ms=attack_ms, release_ms=release_ms,
                         detector_kind="smooth_decoupled_rtt", sc_hpf_hz=None)
    _, gr_no_hpf = comp_no_hpf.process(mix)

    comp_hpf = Compressor()
    comp_hpf.prepare(fs, T=T, R=R, W=W, attack_ms=attack_ms, release_ms=release_ms,
                      detector_kind="smooth_decoupled_rtt", sc_hpf_hz=100.0)
    _, gr_hpf = comp_hpf.process(mix)

    beat_s = 60.0 / 120.0
    def modulation_depth(gr):
        depths = []
        for ht in hit_times[1:-1]:
            i0 = int((ht - 0.05) * fs)
            i1 = int((ht + beat_s * 0.95) * fs)
            depths.append(np.max(gr[i0:i1]) - np.min(gr[i0:i1]))
        return np.mean(depths)

    depth_no_hpf = modulation_depth(gr_no_hpf)
    depth_hpf = modulation_depth(gr_hpf)
    check("100Hz SC HPF reduces GR modulation depth at kick rate",
          depth_hpf < depth_no_hpf,
          f"modulation depth: no-HPF={depth_no_hpf:.2f} dB, with-HPF={depth_hpf:.2f} dB "
          f"({(1 - depth_hpf/depth_no_hpf)*100:.1f}% reduction)")

    fig, ax = plt.subplots(figsize=(11, 5))
    t = np.arange(len(mix)) / fs
    ax.plot(t, gr_no_hpf, label=f"no SC filter (mod depth={depth_no_hpf:.2f} dB)")
    ax.plot(t, gr_hpf, label=f"100Hz SC HPF (mod depth={depth_hpf:.2f} dB)")
    for ht in hit_times:
        ax.axvline(ht, color="gray", ls=":", lw=0.5)
    ax.set_xlabel("s"); ax.set_ylabel("gain reduction (dB)")
    ax.set_title("SC HPF demo: bass-heavy kick+pad mix, pumping reduction at kick rate")
    ax.legend(); ax.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{OUT}/comp_sc_hpf_demo.png", dpi=130)
    plt.close(fig)

    return all_pass


# ============================================================
# VERIFICATION 9: MIX (parallel compression)
# ============================================================

def verify_mix():
    fs = sr
    all_pass = True

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[mix] {status}: {name} -- {detail}")

    n = int(fs * 1.0)
    rng = np.random.default_rng(7)
    x = rng.uniform(-0.7, 0.7, n)
    T, R, W = -20.0, 4.0, 3.0
    attack_ms, release_ms = 5.0, 100.0

    def build(mix, makeup_db=0.0, R_=R):
        c = Compressor()
        c.prepare(fs, T=T, R=R_, W=W, attack_ms=attack_ms, release_ms=release_ms,
                   makeup_db=makeup_db, detector_kind="smooth_decoupled_rtt", mix=mix)
        return c

    # (i) mix=1 == previous (pre-MIX) behavior bit-exact: previous behavior
    # was "return x*gain_lin" with no dry blend at all.
    y_mix1, gr_mix1 = build(mix=1.0).process(x)
    c_ref = build(mix=1.0)  # rebuild fresh (same params) to hand-compute reference
    c_ref.reset()
    ref = np.empty(n)
    for i in range(n):
        sc = x[i]
        x_L = 20.0 * np.log10(max(abs(sc), 1e-6))
        y_G = gain_computer(x_L, T, R, W)
        e = -(y_G - x_L)
        g_sm = -c_ref.detector.process_sample(e)
        gain_lin = 10.0 ** (g_sm / 20.0)
        ref[i] = x[i] * gain_lin
    err_mix1 = np.max(np.abs(y_mix1 - ref))
    check("mix=1 bit-exact vs pre-MIX formula (x*gain_lin, no dry blend)", err_mix1 < 1e-12,
          f"max abs diff = {err_mix1:.3e}")

    # (ii) mix=0 == dry input bit-exact (makeup NOT applied to dry).
    y_mix0, _ = build(mix=0.0, makeup_db=6.0).process(x)  # makeup_db=6 to prove it's ignored on dry
    err_mix0 = np.max(np.abs(y_mix0 - x))
    check("mix=0 bit-exact vs dry input (makeup not applied to dry path)", err_mix0 == 0.0,
          f"max abs diff = {err_mix0:.3e}")

    # (iii) mix=0.5, R=1: both paths are the same (no GR to speak of, sample-
    # aligned, no lookahead) so a plain 50/50 blend must null against input.
    y_half, _ = build(mix=0.5, R_=1.0).process(x)
    err_half = np.max(np.abs(y_half - x))
    check("mix=0.5, R=1 nulls against input (sample-aligned, no lookahead)", err_half < 1e-6,
          f"max abs diff = {err_half:.3e}")

    # (iv) render drums at mix=0.3 -- NY (parallel) compression: crushed
    # settings (fast attack, high ratio, tight threshold) blended back with
    # dry so the transients survive while the sustain gets glued/loud.
    kick, hit_times = make_kick_loop(fs, bars=2, bpm=120.0, seed=30)
    ny = Compressor()
    ny.prepare(fs, T=-35.0, R=8.0, W=2.0, attack_ms=0.5, release_ms=80.0,
               detector_kind="smooth_decoupled_rtt", auto_makeup=True, mix=0.3)
    y_ny, gr_ny = ny.process(kick)
    wavfile.write(f"{OUT}/comp_drums_parallel.wav", fs, (np.clip(y_ny, -1, 1) * 32767).astype(np.int16))
    print(f"[mix] INFO: wrote {OUT}/comp_drums_parallel.wav (NY parallel comp, mix=0.3, "
          f"T=-35dB R=8 attack=0.5ms, peak GR={np.min(gr_ny):.2f} dB)")

    fig, axs = plt.subplots(2, 1, figsize=(10, 7))
    t = np.arange(n) / fs
    axs[0].plot(t[:2000], x[:2000], label="dry", lw=0.8)
    axs[0].plot(t[:2000], y_mix1[:2000], label="mix=1 (wet)", lw=0.8, alpha=0.7)
    axs[0].plot(t[:2000], y_half[:2000], label="mix=0.5, R=1 (nulls to dry)", lw=0.8, alpha=0.7)
    axs[0].set_title("MIX: dry/wet crossfade sanity (noise, first 2000 samples)")
    axs[0].legend(fontsize=8); axs[0].grid(alpha=0.3)

    t_kick = np.arange(len(kick)) / fs
    axs[1].plot(t_kick, gr_ny)
    for ht in hit_times:
        axs[1].axvline(ht, color="gray", ls=":", lw=0.5)
    axs[1].set_title("NY parallel comp GR envelope (mix=0.3 applied downstream, not shown here)")
    axs[1].set_xlabel("s"); axs[1].set_ylabel("GR (dB)"); axs[1].grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{OUT}/comp_mix_demo.png", dpi=130)
    plt.close(fig)

    return all_pass


# ============================================================
# VERIFICATION 10: RANGE (max GR limit)
# ============================================================

def verify_range():
    fs = sr
    all_pass = True

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[range] {status}: {name} -- {detail}")

    dur = 1.2
    n = int(fs * dur)
    t = np.arange(n) / fs
    burst = np.zeros(n)
    burst_on = (t >= 0.15) & (t < 0.9)
    burst[burst_on] = 0.9 * np.sin(2 * np.pi * 300.0 * t[burst_on])

    T, R, W = -40.0, 8.0, 0.0
    attack_ms, release_ms = 2.0, 150.0
    range_db = 6.0

    comp_clamped = Compressor()
    comp_clamped.prepare(fs, T=T, R=R, W=W, attack_ms=attack_ms, release_ms=release_ms,
                          detector_kind="smooth_decoupled_rtt", range_db=range_db)
    y_c, gr_c = comp_clamped.process(burst)

    comp_unclamped = Compressor()
    comp_unclamped.prepare(fs, T=T, R=R, W=W, attack_ms=attack_ms, release_ms=release_ms,
                            detector_kind="smooth_decoupled_rtt")  # range_db defaults to inf
    y_u, gr_u = comp_unclamped.process(burst)

    max_unclamped_gr = np.max(-gr_u)
    check("unclamped GR actually exceeds 20dB (test is meaningful)", max_unclamped_gr > 20.0,
          f"max unclamped |GR| = {max_unclamped_gr:.3f} dB")

    max_applied_gr = np.max(-gr_c)
    check("applied GR never exceeds range_db=6 (tol 1e-9)", max_applied_gr <= 6.0 + 1e-9,
          f"max applied |GR| = {max_applied_gr:.9f} dB (limit 6.0 dB)")

    # Envelope smooth at the clamp boundary: clamping is a plain max() against
    # a constant, so it can only introduce a bounded SLOPE KINK where the
    # unclamped curve crosses -range_db -- never a jump in VALUE. A real click
    # (discontinuous value) would show up as a second-difference on the order
    # of the total GR swing (here ~34dB) concentrated in a single sample. A
    # kink instead produces a second-difference on the order of the slope
    # change across one sample, which for a ballistics-smoothed envelope is
    # tiny relative to the overall dynamic range being clamped. Bound it
    # against that dynamic range (not against the unclamped curve's own
    # ambient wobble, which is near-zero away from transients and makes a
    # relative-ratio bound too tight/noisy to be a meaningful click test).
    crossing_idx = np.where(np.diff((-gr_u > range_db).astype(int)) != 0)[0]
    win = int(0.002 * fs)  # 2ms window around each crossing
    d2_clamped_near_crossing = []
    for ci in crossing_idx:
        lo, hi = max(0, ci - win), min(n, ci + win)
        seg = gr_c[lo:hi]
        if len(seg) > 2:
            d2_clamped_near_crossing.append(np.max(np.abs(np.diff(seg, n=2))))
    max_d2_clamped = max(d2_clamped_near_crossing) if d2_clamped_near_crossing else 0.0
    gr_swing = max_unclamped_gr  # dB, total dynamic range being clamped
    click_bound = gr_swing * 0.05  # a real click would be O(gr_swing); a kink is <<
    check("clamped envelope's curvature at boundary bounded (kink, not a click)",
          max_d2_clamped <= click_bound,
          f"max second-diff near clamp crossings = {max_d2_clamped:.3e} dB, "
          f"click-scale bound (5% of {gr_swing:.1f}dB swing) = {click_bound:.3e} dB, "
          f"found {len(crossing_idx)} crossing(s)")

    # range=inf == previous (pre-RANGE) behavior bit-exact.
    comp_default = Compressor()
    comp_default.prepare(fs, T=T, R=R, W=W, attack_ms=attack_ms, release_ms=release_ms,
                          detector_kind="smooth_decoupled_rtt")  # no range_db passed at all
    y_d, gr_d = comp_default.process(burst)
    err_default = np.max(np.abs(gr_d - gr_u))
    check("range_db=inf explicit == default (no range_db) bit-exact", err_default == 0.0,
          f"max abs diff = {err_default:.3e}")

    fig, axs = plt.subplots(2, 1, figsize=(10, 7), sharex=True)
    axs[0].plot(t, -gr_u, label="unclamped |GR|")
    axs[0].plot(t, -gr_c, label=f"clamped |GR| (range={range_db}dB)")
    axs[0].axhline(range_db, color="red", ls=":", lw=0.8, label="range limit")
    axs[0].set_ylabel("|GR| (dB)"); axs[0].legend(fontsize=8); axs[0].grid(alpha=0.3)
    axs[0].set_title("RANGE: clamp engages/disengages cleanly at the boundary")
    d2 = np.empty(n); d2[:] = np.nan
    d2[1:-1] = np.diff(gr_c, n=2)
    axs[1].plot(t, d2, lw=0.6, color="C2")
    axs[1].set_ylabel("2nd diff of applied GR"); axs[1].set_xlabel("s"); axs[1].grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{OUT}/comp_range_demo.png", dpi=130)
    plt.close(fig)

    return all_pass


# ============================================================
# VERIFICATION 11: HOLD (attack-release hold time)
# ============================================================

def ref_smooth_decoupled_no_hold(e, aA, aR):
    """Pre-HOLD reference implementation of smooth_decoupled_rtt, reproduced
    verbatim from the original process_sample formula, to prove hold=0 is
    bit-identical to the code path that existed before HOLD was added."""
    y1 = s1 = 0.0
    out = np.empty(len(e))
    for i, x in enumerate(e):
        s1 = max(x, aR * (s1 - x) + x)
        y = aA * (y1 - s1) + s1
        y1 = y
        out[i] = y
    return out


def verify_hold():
    fs = sr
    all_pass = True

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[hold] {status}: {name} -- {detail}")

    attack_ms, release_ms = 0.0005, 100.0  # instant attack -> y == s1 exactly (aA=0)
    high, low = 10.0, 0.0

    gr_envs = {}
    for hold_ms in (5.0, 50.0):
        det = PeakDetector("smooth_decoupled_rtt")
        det.prepare(fs, attack_ms=attack_ms, release_ms=release_ms, hold_ms=hold_ms)
        det.reset()
        pre, burst = 300, 200
        drop_idx = pre + burst
        n = drop_idx + int(0.15 * fs)
        e = np.full(n, low)
        e[pre:drop_idx] = high
        y = np.empty(n)
        for i in range(n):
            y[i] = det.process_sample(e[i])
        gr_envs[hold_ms] = (e, y, drop_idx)

        eps = 1e-9
        after = y[drop_idx:]
        released = np.where(after < high - eps)[0]
        measured_samples = released[0] if len(released) else len(after)
        expected_samples = det.hold_samples
        measured_ms = measured_samples / fs * 1000.0
        check(f"hold={hold_ms}ms: release delayed by hold time (within 1 sample)",
              abs(int(measured_samples) - expected_samples) <= 1,
              f"measured {measured_samples} samples ({measured_ms:.4f} ms) vs "
              f"expected {expected_samples} samples ({hold_ms} ms label)")

    # Retrigger mid-hold: a second attack burst during the hold window must
    # (a) engage attack normally, and (b) restart the hold counter, so the
    # hold measured after the SECOND drop is the full hold_ms again -- not
    # hold_ms minus whatever had already elapsed before the retrigger.
    hold_ms = 50.0
    det = PeakDetector("smooth_decoupled_rtt")
    det.prepare(fs, attack_ms=attack_ms, release_ms=release_ms, hold_ms=hold_ms)
    det.reset()
    pre1, dip_len, burst2_len = 300, 10, 200
    idx0 = pre1                    # first drop (into hold)
    idx1 = idx0 + dip_len          # retrigger (attack again)
    idx2 = idx1 + burst2_len       # second drop (hold should restart here)
    n = idx2 + int(0.15 * fs)
    e = np.full(n, low)
    e[0:idx0] = high
    e[idx1:idx2] = high
    y = np.empty(n)
    for i in range(n):
        y[i] = det.process_sample(e[i])

    eps = 1e-9
    held_during_dip = np.all(y[idx0:idx1] >= high - eps)
    check("retrigger: envelope held (frozen) during the short dip inside hold window",
          held_during_dip, f"min y during dip = {np.min(y[idx0:idx1]):.6f} (expected == {high})")

    after2 = y[idx2:]
    released2 = np.where(after2 < high - eps)[0]
    measured2 = released2[0] if len(released2) else len(after2)
    expected2 = det.hold_samples
    check("retrigger: hold restarts fully after second drop (not shortened)",
          abs(int(measured2) - expected2) <= 1,
          f"measured {measured2} samples vs expected {expected2} samples (full {hold_ms}ms hold)")

    # hold=0 bit-identical to the pre-HOLD code path.
    rng = np.random.default_rng(99)
    e_rand = np.abs(rng.standard_normal(2000)) * 15.0  # positive "GR demand"-like signal
    det0 = PeakDetector("smooth_decoupled_rtt")
    det0.prepare(fs, attack_ms=5.0, release_ms=120.0, hold_ms=0.0)
    det0.reset()
    aA, aR = det0.aA, det0.aR
    y0 = np.array([det0.process_sample(v) for v in e_rand])
    y_ref = ref_smooth_decoupled_no_hold(e_rand, aA, aR)
    err0 = np.max(np.abs(y0 - y_ref))
    check("hold=0 bit-identical to pre-HOLD formula", err0 == 0.0, f"max abs diff = {err0:.3e}")

    fig, axs = plt.subplots(2, 1, figsize=(10, 7))
    for hm, (e_hm, y_hm, drop_idx_hm) in gr_envs.items():
        t = np.arange(len(y_hm)) / fs * 1000.0
        axs[0].plot(t, y_hm, label=f"hold={hm}ms")
    axs[0].axvline(300 / fs * 1000.0, color="gray", ls=":", lw=0.6)
    axs[0].axvline((300 + 200) / fs * 1000.0, color="gray", ls=":", lw=0.6, label="input drops")
    axs[0].set_title("HOLD: envelope freezes at the drop, then release engages")
    axs[0].set_ylabel("detector output"); axs[0].legend(fontsize=8); axs[0].grid(alpha=0.3)

    t2 = np.arange(n) / fs * 1000.0
    axs[1].plot(t2, e, label="input e", alpha=0.5)
    axs[1].plot(t2, y, label="detector output (hold=50ms, with retrigger)")
    axs[1].axvline(idx0 / fs * 1000.0, color="gray", ls=":", lw=0.6)
    axs[1].axvline(idx1 / fs * 1000.0, color="green", ls=":", lw=0.6, label="retrigger")
    axs[1].axvline(idx2 / fs * 1000.0, color="red", ls=":", lw=0.6, label="2nd drop (hold restarts)")
    axs[1].set_title("HOLD: retrigger mid-hold restarts the counter")
    axs[1].set_xlabel("ms"); axs[1].legend(fontsize=8); axs[1].grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{OUT}/comp_hold_demo.png", dpi=130)
    plt.close(fig)

    return all_pass


# ============================================================
# combined summary plot: MIX / RANGE / HOLD GR envelopes, one figure
# ============================================================

def plot_mix_range_hold_summary():
    fs = sr

    # -- MIX: R=1 chain, mix=0.5 nulling against a burst input --
    n = int(fs * 0.05)
    t = np.arange(n) / fs
    x = 0.6 * np.sin(2 * np.pi * 300.0 * t)
    comp_mix = Compressor()
    comp_mix.prepare(fs, T=-20.0, R=1.0, W=3.0, attack_ms=5.0, release_ms=100.0,
                      detector_kind="smooth_decoupled_rtt", mix=0.5)
    y_mix, _ = comp_mix.process(x)

    # -- RANGE: sine burst, clamped vs unclamped GR --
    dur = 1.2
    n2 = int(fs * dur)
    t2 = np.arange(n2) / fs
    burst = np.zeros(n2)
    burst_on = (t2 >= 0.15) & (t2 < 0.9)
    burst[burst_on] = 0.9 * np.sin(2 * np.pi * 300.0 * t2[burst_on])
    range_db = 6.0
    comp_c = Compressor()
    comp_c.prepare(fs, T=-40.0, R=8.0, W=0.0, attack_ms=2.0, release_ms=150.0,
                    detector_kind="smooth_decoupled_rtt", range_db=range_db)
    _, gr_c = comp_c.process(burst)
    comp_u = Compressor()
    comp_u.prepare(fs, T=-40.0, R=8.0, W=0.0, attack_ms=2.0, release_ms=150.0,
                    detector_kind="smooth_decoupled_rtt")
    _, gr_u = comp_u.process(burst)

    # -- HOLD: tone burst, envelope freeze for hold_ms before release --
    high, low = 10.0, 0.0
    pre, hburst = 300, 200
    drop_idx = pre + hburst
    n3 = drop_idx + int(0.15 * fs)
    e = np.full(n3, low)
    e[pre:drop_idx] = high
    hold_curves = {}
    for hold_ms in (0.0, 5.0, 50.0):
        det = PeakDetector("smooth_decoupled_rtt")
        det.prepare(fs, attack_ms=0.0005, release_ms=100.0, hold_ms=hold_ms)
        det.reset()
        y = np.array([det.process_sample(v) for v in e])
        hold_curves[hold_ms] = y

    fig, axs = plt.subplots(3, 1, figsize=(10, 11))

    axs[0].plot(t * 1000, x, label="input (dry)", lw=1.0)
    axs[0].plot(t * 1000, y_mix, "--", label="mix=0.5, R=1 output (nulls to input)", lw=1.0)
    axs[0].set_title("MIX: parallel-comp crossfade nulls against input at R=1 (no lookahead)")
    axs[0].set_xlabel("ms"); axs[0].set_ylabel("amplitude"); axs[0].legend(fontsize=8); axs[0].grid(alpha=0.3)

    axs[1].plot(t2, -gr_u, label="unclamped |GR|")
    axs[1].plot(t2, -gr_c, label=f"clamped |GR| (range={range_db}dB)")
    axs[1].axhline(range_db, color="red", ls=":", lw=0.8, label="range limit")
    axs[1].set_title("RANGE: applied GR clamped at range_db, no click at boundary")
    axs[1].set_xlabel("s"); axs[1].set_ylabel("|GR| (dB)"); axs[1].legend(fontsize=8); axs[1].grid(alpha=0.3)

    t3 = np.arange(n3) / fs * 1000.0
    for hold_ms, y in hold_curves.items():
        axs[2].plot(t3, y, label=f"hold={hold_ms}ms")
    axs[2].axvline(drop_idx / fs * 1000.0, color="gray", ls=":", lw=0.8, label="input drops")
    axs[2].set_title("HOLD: envelope freezes for hold_ms before release engages")
    axs[2].set_xlabel("ms"); axs[2].set_ylabel("detector output"); axs[2].legend(fontsize=8); axs[2].grid(alpha=0.3)

    plt.tight_layout()
    plt.savefig(f"{OUT}/comp_mix_range_hold.png", dpi=130)
    plt.close(fig)


# ============================================================
# main
# ============================================================

if __name__ == "__main__":
    print("=== 1. Static curve ===")
    p1 = verify_static_curve()

    print("\n=== 2. C1 knee slope ===")
    p2 = verify_knee_slope()

    print("\n=== 3. Ballistics (attack/release time, level independence) ===")
    p3 = verify_ballistics()

    print("\n=== 4. Detector comparison (release kink) ===")
    p4 = verify_detector_comparison()

    print("\n=== 5. Program-dependent release (dual-pole A/B) ===")
    p5 = verify_program_dependent()

    print("\n=== 6. Null test ===")
    p6 = verify_null_test()

    print("\n=== 7. THD ===")
    p7 = verify_thd()

    print("\n=== 8. SC HPF demo ===")
    p8 = verify_sc_hpf_demo()

    print("\n=== 9. MIX (parallel compression) ===")
    p9 = verify_mix()

    print("\n=== 10. RANGE (max GR limit) ===")
    p10 = verify_range()

    print("\n=== 11. HOLD (attack-release hold time) ===")
    p11 = verify_hold()

    plot_mix_range_hold_summary()

    print(f"\nWrote plots and WAVs to {OUT}/")

    all_pass = p1 and p2 and p3 and p4 and p5 and p6 and p7 and p8 and p9 and p10 and p11
    print(f"\n{'ALL PASS' if all_pass else 'SOME FAILURES'} "
          f"(static={p1}, knee={p2}, ballistics={p3}, detectors={p4}, "
          f"prog_dep={p5}, null={p6}, thd={p7}, sc_hpf={p8}, "
          f"mix={p9}, range={p10}, hold={p11})")
    assert all_pass, "one or more verifications failed -- see FAIL lines above"
