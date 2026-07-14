"""16-band Pro-Q-class parametric EQ: SVF band types, variable-slope
Butterworth cascades, and a click-free enable/disable crossfade.

Part 1: the 8 SVF band types (bell, low/high shelf, tilt, notch, bandpass,
        lowpass, highpass) sharing ONE state-variable-filter kernel -- only
        the (g, k, m0, m1, m2) coefficient recipe changes per type. Verified
        against known closed-form behavior (peak gain, asymptotes, -3dB
        points) via impulse-response FFT, since the SVF's recurrence is our
        ground truth rather than a textbook transfer function.
Part 2: turn a single 12dB/oct SVF section into a variable-slope filter
        (6..96 dB/oct) by cascading floor(N/2) second-order sections (all
        sharing the same g, i.e. the same cutoff, but each with its own
        Butterworth pole-angle k) plus one first-order TPT section when the
        order N is odd. Verified against scipy.signal.butter+freqz, which
        is a completely independent (matrix-based) implementation of the
        same bilinear-transformed Butterworth design -- if our hand-rolled
        cascade matches it to << 0.1 dB, the cascade math is right.
Part 3: wire 16 of these bands in series (a "channel strip"), and give each
        band a 10ms linear crossfade so flipping enabled/disabled mid-signal
        doesn't click. Contrasted against a hard (unfaded) toggle so the
        click is visible/audible as a control.
"""
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, freqz, sosfreqz
import matplotlib.pyplot as plt
import os

sr = 44100
OUT = "proto/out"
os.makedirs(OUT, exist_ok=True)


# ============================================================
# PART 1: the 8 SVF band types on one shared kernel
# ============================================================

def svf_coeffs(fs, f0, dBgain, Q, band_type):
    """Compute (g, k, a1, a2, a3, m0, m1, m2) for one SVF section.

    g is the (pre-warped) tangent of the cutoff -- it's what "tunes" the
    filter. k is effectively 1/Q, controlling resonance/slope shape. m0/m1/
    m2 are just a *linear combination* of the three internal signals
    (v0=input, v1=bandpass-ish, v2=lowpass-ish) that happens to produce
    the desired band shape -- the SAME two integrators underneath every
    type, only the readout differs. That's why one kernel serves 8 shapes.
    """
    A = 10 ** (dBgain / 40)
    if band_type == "bell":
        g = np.tan(np.pi * f0 / fs)
        k = 1 / (Q * A)
        m0, m1, m2 = 1.0, k * (A * A - 1), 0.0
    elif band_type == "low_shelf":
        g = np.tan(np.pi * f0 / fs) / np.sqrt(A)
        k = 1 / Q
        m0, m1, m2 = 1.0, k * (A - 1), A * A - 1
    elif band_type == "high_shelf":
        g = np.tan(np.pi * f0 / fs) * np.sqrt(A)
        k = 1 / Q
        m0, m1, m2 = A * A, k * (1 - A) * A, 1 - A * A
    elif band_type == "tilt":
        g = np.tan(np.pi * f0 / fs) * np.sqrt(A)
        k = 1 / Q
        m0, m1, m2 = A, k * (1 - A), 1 / A - A
    elif band_type == "notch":
        g = np.tan(np.pi * f0 / fs)
        k = 1 / Q
        m0, m1, m2 = 1.0, -k, 0.0
    elif band_type == "bandpass":
        g = np.tan(np.pi * f0 / fs)
        k = 1 / Q
        m0, m1, m2 = 0.0, k, 0.0
    elif band_type == "lowpass":
        g = np.tan(np.pi * f0 / fs)
        k = 1 / Q
        m0, m1, m2 = 0.0, 0.0, 1.0
    elif band_type == "highpass":
        g = np.tan(np.pi * f0 / fs)
        k = 1 / Q
        m0, m1, m2 = 1.0, -k, -1.0
    else:
        raise ValueError(f"unknown band_type {band_type}")

    a1 = 1 / (1 + g * (g + k))
    a2 = g * a1
    a3 = g * a2
    return g, k, a1, a2, a3, m0, m1, m2


class SVFSection:
    """One state-variable-filter section: 2 state vars, any of the 8 types.

    process_sample() is the audio-thread-safe primitive -- no allocation,
    fixed work per call. process() (block-at-a-time, for offline/plot use
    here) just loops it.
    """

    def __init__(self):
        self.ic1eq = 0.0
        self.ic2eq = 0.0
        self.a1 = self.a2 = self.a3 = 0.0
        self.m0 = self.m1 = self.m2 = 0.0

    def prepare(self, fs, f0, dBgain, Q, band_type):
        _, _, a1, a2, a3, m0, m1, m2 = svf_coeffs(fs, f0, dBgain, Q, band_type)
        self.a1, self.a2, self.a3 = a1, a2, a3
        self.m0, self.m1, self.m2 = m0, m1, m2

    def reset(self):
        self.ic1eq = 0.0
        self.ic2eq = 0.0

    def process_sample(self, v0):
        a1, a2, a3 = self.a1, self.a2, self.a3
        m0, m1, m2 = self.m0, self.m1, self.m2
        ic1eq, ic2eq = self.ic1eq, self.ic2eq

        v3 = v0 - ic2eq
        v1 = a1 * ic1eq + a2 * v3
        v2 = ic2eq + a2 * ic1eq + a3 * v3
        ic1eq = 2 * v1 - ic1eq
        ic2eq = 2 * v2 - ic2eq

        self.ic1eq, self.ic2eq = ic1eq, ic2eq
        return m0 * v0 + m1 * v1 + m2 * v2

    def process(self, x):
        y = np.empty_like(x, dtype=np.float64)
        for i in range(len(x)):
            y[i] = self.process_sample(x[i])
        return y


def measure_response(make_section, fs, n=32768):
    """Impulse -> FFT magnitude in dB. `make_section` is a zero-arg factory
    returning a fresh, prepared filter object (so state starts at rest).
    """
    sec = make_section()
    impulse = np.zeros(n)
    impulse[0] = 1.0
    ir = sec.process(impulse)
    H = np.fft.rfft(ir)
    freqs = np.fft.rfftfreq(n, d=1 / fs)
    mag_db = 20 * np.log10(np.abs(H) + 1e-15)
    return freqs, mag_db


def db_at(freqs, mag_db, f_target):
    idx = np.argmin(np.abs(freqs - f_target))
    return mag_db[idx]


def verify_part1():
    fs = sr
    f0 = 1000.0
    Q = 1.0
    dBgain = 12.0
    all_pass = True

    fig, axs = plt.subplots(2, 4, figsize=(18, 8))
    axs = axs.ravel()
    types = ["bell", "low_shelf", "high_shelf", "tilt",
             "notch", "bandpass", "lowpass", "highpass"]

    for ax, band_type in zip(axs, types):
        def factory(bt=band_type, q=Q):
            s = SVFSection()
            s.prepare(fs, f0, dBgain, q, bt)
            return s
        freqs, mag = measure_response(factory, fs)
        ax.semilogx(freqs[1:], mag[1:])
        ax.set_title(band_type)
        ax.set_xlim(20, fs / 2)
        ax.set_ylim(-30, 20)
        ax.grid(True, which="both", alpha=0.3)
        ax.axvline(f0, color="gray", lw=0.5, ls=":")
    plt.tight_layout()
    plt.savefig(f"{OUT}/part1_band_types_grid.png", dpi=130)
    plt.close(fig)

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[part1] {status}: {name} -- {detail}")

    # --- bell: peak == dBgain at f0 ---
    for g_db in (-12.0, 6.0, 12.0):
        def factory(g_db=g_db):
            s = SVFSection(); s.prepare(fs, f0, g_db, 1.0, "bell"); return s
        freqs, mag = measure_response(factory, fs)
        peak = db_at(freqs, mag, f0)
        check(f"bell peak @ {g_db:+.0f} dB", abs(peak - g_db) < 0.15,
              f"measured {peak:.3f} dB at f0")

    # --- shelves: asymptotes ---
    def factory_ls():
        s = SVFSection(); s.prepare(fs, f0, dBgain, 0.7071, "low_shelf"); return s
    freqs, mag = measure_response(factory_ls, fs)
    lo, hi = db_at(freqs, mag, 20), db_at(freqs, mag, fs / 2 * 0.95)
    check("low_shelf asymptotes", abs(lo - dBgain) < 0.3 and abs(hi) < 0.3,
          f"low={lo:.3f} dB (want {dBgain:.0f}), high={hi:.3f} dB (want 0)")

    def factory_hs():
        s = SVFSection(); s.prepare(fs, f0, dBgain, 0.7071, "high_shelf"); return s
    freqs, mag = measure_response(factory_hs, fs)
    lo, hi = db_at(freqs, mag, 20), db_at(freqs, mag, fs / 2 * 0.95)
    check("high_shelf asymptotes", abs(lo) < 0.3 and abs(hi - dBgain) < 0.3,
          f"low={lo:.3f} dB (want 0), high={hi:.3f} dB (want {dBgain:.0f})")

    # --- tilt: CRITICAL new derivation ---
    def factory_tilt():
        s = SVFSection(); s.prepare(fs, f0, dBgain, 0.7071, "tilt"); return s
    freqs, mag = measure_response(factory_tilt, fs)
    pivot = db_at(freqs, mag, f0)
    lo, hi = db_at(freqs, mag, 20), db_at(freqs, mag, fs / 2 * 0.95)
    check("tilt pivot @ f0 == 0 dB", abs(pivot) < 0.2, f"measured {pivot:.4f} dB")
    check("tilt low asymptote == -gain/2", abs(lo - (-dBgain / 2)) < 0.3,
          f"measured {lo:.3f} dB (want {-dBgain/2:.1f})")
    check("tilt high asymptote == +gain/2", abs(hi - dBgain / 2) < 0.3,
          f"measured {hi:.3f} dB (want {dBgain/2:.1f})")

    # --- notch: deep null at f0, flat far away ---
    def factory_notch():
        s = SVFSection(); s.prepare(fs, f0, 0.0, 4.0, "notch"); return s
    freqs, mag = measure_response(factory_notch, fs)
    null_db = db_at(freqs, mag, f0)
    far_db = db_at(freqs, mag, 100.0)
    check("notch null depth", null_db < -40,
          f"null={null_db:.1f} dB at f0, far={far_db:.3f} dB at 100 Hz")

    # --- bandpass: exactly 0 dB peak at f0 for several Q ---
    for q in (0.5, 1.0, 2.0, 5.0, 10.0):
        def factory(q=q):
            s = SVFSection(); s.prepare(fs, f0, 0.0, q, "bandpass"); return s
        freqs, mag = measure_response(factory, fs)
        peak = np.max(mag)
        check(f"bandpass unit peak, Q={q}", abs(peak) < 0.05,
              f"peak={peak:.4f} dB (want 0)")

    # --- LP/HP: -3dB at f0 for Q=0.7071, correct rolloff direction ---
    def factory_lp():
        s = SVFSection(); s.prepare(fs, f0, 0.0, 0.7071, "lowpass"); return s
    freqs, mag = measure_response(factory_lp, fs)
    cut = db_at(freqs, mag, f0)
    lo, hi = db_at(freqs, mag, 100), db_at(freqs, mag, fs / 2 * 0.9)
    check("lowpass -3dB @ f0", abs(cut - (-3.0)) < 0.3, f"measured {cut:.3f} dB")
    check("lowpass rolloff direction", lo > cut > hi,
          f"low={lo:.2f} > cut={cut:.2f} > high={hi:.2f}")

    def factory_hp():
        s = SVFSection(); s.prepare(fs, f0, 0.0, 0.7071, "highpass"); return s
    freqs, mag = measure_response(factory_hp, fs)
    cut = db_at(freqs, mag, f0)
    lo, hi = db_at(freqs, mag, 100), db_at(freqs, mag, fs / 2 * 0.9)
    check("highpass -3dB @ f0", abs(cut - (-3.0)) < 0.3, f"measured {cut:.3f} dB")
    check("highpass rolloff direction", hi > cut > lo,
          f"high={hi:.2f} > cut={cut:.2f} > low={lo:.2f}")

    return all_pass


# ============================================================
# PART 2: variable-slope Butterworth cascades (6..96 dB/oct)
# ============================================================

class FirstOrderTPT:
    """First-order TPT one-pole -- the odd-order leftover of a Butterworth
    cascade (e.g. order 3 = one 2nd-order section + this).
    State s is again a physical integrator state, not delay-line history,
    for the same reason the 2nd-order SVF above tolerates live coeff swaps.
    """

    def __init__(self):
        self.s = 0.0
        self.G = 0.0
        self.mode = "lowpass"

    def prepare(self, fs, f0, mode):
        g = np.tan(np.pi * f0 / fs)
        self.G = g / (1 + g)
        self.mode = mode

    def reset(self):
        self.s = 0.0

    def process_sample(self, x):
        v = (x - self.s) * self.G
        lp = v + self.s
        self.s = lp + v
        hp = x - lp
        return lp if self.mode == "lowpass" else hp


class ButterworthCascade:
    """Order-N Butterworth LP/HP built from floor(N/2) SVF sections (shared
    cutoff g, individual pole-angle k_i) plus one first-order TPT section
    when N is odd.

    WHY per-section k differs: a Butterworth filter's N poles sit on a
    circle in the s-plane, evenly spaced by angle. Each 2nd-order section
    realizes one conjugate pole pair; k_i = 2*cos(pole angle) is exactly the
    "distance from the unit circle" for that pair, i.e. each section gets
    its own resonance so that, cascaded, the ripple-free maximally-flat
    Butterworth shape falls out.
    """

    def __init__(self):
        self.sections = []  # list of SVFSection
        self.first_order = None
        self.N = None
        self.btype = None

    def prepare(self, fs, f0, N, btype):
        self.N = N
        self.btype = btype
        n_pairs = N // 2
        self.sections = []
        for i in range(1, n_pairs + 1):
            # NOTE: the spec's k_i = 2*cos((2i-1)pi/(2N)) matches scipy's
            # Butterworth poles for EVEN N (cos/sin land on the same value
            # set there by index symmetry) but is measurably wrong for ODD
            # N -- cross-checked directly against scipy.signal.buttap(N)'s
            # analog pole angles. The correct universal pole-angle formula
            # is k_i = 2*sin((2i-1)*pi/(2N)); it reduces to the same numbers
            # as the spec's cos form for every even N tested (2,4,6,8) and
            # is what actually reproduces scipy's poles for N=3. Verified
            # against scipy.signal.butter+freqz below (< 0.1 dB error).
            k_i = 2 * np.sin((2 * i - 1) * np.pi / (2 * N))
            g, k, a1, a2, a3, m0, m1, m2 = svf_coeffs(fs, f0, 0.0, 1.0 / k_i, btype)
            sec = SVFSection()
            sec.a1, sec.a2, sec.a3 = a1, a2, a3
            sec.m0, sec.m1, sec.m2 = m0, m1, m2
            self.sections.append(sec)
        if N % 2 == 1:
            self.first_order = FirstOrderTPT()
            self.first_order.prepare(fs, f0, btype)
        else:
            self.first_order = None

    def set_last_section_k(self, fs, f0, k_new):
        """Q-knob: override the LAST section's k (and its a1/a2/a3, m1 for
        highpass) while leaving every earlier section untouched -- this is
        what lets a UI "resonance" knob add a bump at cutoff without
        disturbing the rest of the slope.
        """
        g, k, a1, a2, a3, m0, m1, m2 = svf_coeffs(fs, f0, 0.0, 1.0 / k_new, self.btype)
        sec = self.sections[-1]
        sec.a1, sec.a2, sec.a3 = a1, a2, a3
        sec.m0, sec.m1, sec.m2 = m0, m1, m2

    def reset(self):
        for sec in self.sections:
            sec.reset()
        if self.first_order is not None:
            self.first_order.reset()

    def process_sample(self, x):
        y = x
        for sec in self.sections:
            y = sec.process_sample(y)
        if self.first_order is not None:
            y = self.first_order.process_sample(y)
        return y

    def process(self, x):
        y = np.empty_like(x, dtype=np.float64)
        for i in range(len(x)):
            y[i] = self.process_sample(x[i])
        return y


def verify_part2_slopes():
    fs = sr
    f0 = 1000.0
    all_pass = True
    results = {}

    for N in (1, 2, 3, 4, 8, 12, 16):
        for btype, sp_type in (("lowpass", "low"), ("highpass", "high")):
            def factory(N=N, btype=btype):
                c = ButterworthCascade()
                c.prepare(fs, f0, N, btype)
                return c
            freqs, mag_cascade = measure_response(factory, fs, n=32768)

            # output='sos' + sosfreqz: scipy's plain transfer-function 'ba'
            # form loses so much numerical precision above ~order 10 that
            # its own coefficients become an inaccurate ground truth (we
            # saw the 'ba' reference itself reporting +36 dB in a supposed
            # deep-stopband region for N=16 highpass -- that's scipy's
            # polynomial coefficients underflowing/overflowing, not our
            # cascade). SOS is the numerically stable factored form, same
            # design, and is the right independent reference to check against.
            sos = butter(N, f0, btype=sp_type, fs=fs, output="sos")
            w, h = sosfreqz(sos, worN=freqs[freqs <= fs / 2], fs=fs)
            mag_ref = 20 * np.log10(np.abs(h) + 1e-15)
            mag_cascade_i = np.interp(w, freqs, mag_cascade)

            # Deep in the stopband both curves approach -inf dB; a tiny
            # floating-point difference there gets blown up into a huge dB
            # error even though the LINEAR difference is negligible. Floor
            # both curves before comparing -- what matters is the pass-band
            # and transition, not disagreement 150dB down in the noise.
            FLOOR_DB = -100.0
            mag_c_floor = np.maximum(mag_cascade_i, FLOOR_DB)
            mag_r_floor = np.maximum(mag_ref, FLOOR_DB)

            band = w < 0.9 * (fs / 2)
            err = np.max(np.abs(mag_c_floor[band] - mag_r_floor[band]))
            results[(N, btype)] = err

            slope_db_oct = N * 6
            status = "PASS" if err < 0.1 else "FAIL"
            if err >= 0.1:
                all_pass = False
            print(f"[part2] {status}: N={N:>2} ({slope_db_oct} dB/oct) {btype:9s} "
                  f"vs scipy.butter -- max err below 0.9*Nyquist = {err:.4f} dB")

    return all_pass, results


def verify_part2_q_knob():
    """Scale ONLY the last section's k of a 4th-order LP; slope/far-stopband
    should stay put while a resonant bump grows at cutoff.
    """
    fs = sr
    f0 = 1000.0
    N = 4
    Q_base = 0.7071
    Q_users = (0.7071, 2.0, 8.0)

    fig, ax = plt.subplots(figsize=(8, 5))
    peaks = {}
    far_stopband = {}
    for Q_user in Q_users:
        cascade = ButterworthCascade()
        cascade.prepare(fs, f0, N, "lowpass")
        k_last_base = 2 * np.sin((2 * 2 - 1) * np.pi / (2 * N))  # i=2 (last of 2 pairs)
        k_new = k_last_base * (Q_base / Q_user)
        cascade.set_last_section_k(fs, f0, k_new)

        freqs, mag = measure_response(lambda c=cascade: c, fs, n=32768)
        # measure_response resets state via a fresh factory normally; here we
        # already have a prepared-but-unused cascade, so state is at rest.
        ax.semilogx(freqs[1:], mag[1:], label=f"Q_user={Q_user}")

        peak_near_cutoff = np.max(mag[(freqs > 500) & (freqs < 2000)])
        far = db_at(freqs, mag, f0 * 8)
        peaks[Q_user] = peak_near_cutoff
        far_stopband[Q_user] = far

    ax.set_title(f"4th-order LP, f0={f0} Hz -- Q-knob scales only the last section's k")
    ax.set_xlabel("Hz"); ax.set_ylabel("dB")
    ax.set_xlim(20, fs / 2); ax.set_ylim(-60, 20)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    plt.tight_layout()
    plt.savefig(f"{OUT}/part2_q_knob.png", dpi=130)
    plt.close(fig)

    all_pass = True
    print(f"[part2] Q-knob peak near cutoff: " +
          ", ".join(f"Q={q}: {peaks[q]:.2f} dB" for q in Q_users))
    monotonic = peaks[0.7071] < peaks[2.0] < peaks[8.0]
    status = "PASS" if monotonic else "FAIL"
    if not monotonic:
        all_pass = False
    print(f"[part2] {status}: resonant bump grows monotonically with Q_user")

    far_spread = max(far_stopband.values()) - min(far_stopband.values())
    status = "PASS" if far_spread < 0.5 else "FAIL"
    if far_spread >= 0.5:
        all_pass = False
    print(f"[part2] {status}: far stopband (8*f0) unchanged across Q -- "
          f"spread={far_spread:.4f} dB " +
          ", ".join(f"Q={q}: {far_stopband[q]:.2f} dB" for q in Q_users))

    return all_pass


# ============================================================
# PART 3: 16-band cascade + smoothed bypass crossfade
# ============================================================

class EQBand:
    """One band in the channel strip: either a single SVF section (bell,
    shelf, tilt, notch, bandpass) or a variable-slope Butterworth cascade
    (lowpass/highpass, where `slope` sets the order).
    """

    def __init__(self, band_type, f0, Q, gain_db, slope=12.0, enabled=True):
        self.band_type = band_type
        self.f0 = f0
        self.Q = Q
        self.gain_db = gain_db
        self.slope = slope
        self.enabled = enabled
        self._impl = None

    def prepare(self, fs):
        if self.band_type in ("lowpass", "highpass"):
            N = max(1, round(self.slope / 6))
            impl = ButterworthCascade()
            impl.prepare(fs, self.f0, N, self.band_type)
        else:
            impl = SVFSection()
            impl.prepare(fs, self.f0, self.gain_db, self.Q, self.band_type)
        self._impl = impl

    def reset(self):
        self._impl.reset()

    def process_sample(self, x):
        return self._impl.process_sample(x)


DEFAULT_16_BANDS = [
    EQBand("highpass", 30.0, 0.7071, 0.0, slope=24.0, enabled=True),
    EQBand("low_shelf", 120.0, 0.7071, 3.0, enabled=True),
    EQBand("bell", 350.0, 2.0, -4.0, enabled=True),
    EQBand("bell", 3000.0, 1.0, 2.5, enabled=True),
    EQBand("tilt", 1000.0, 0.7071, 2.0, enabled=True),
    EQBand("high_shelf", 10000.0, 0.7071, -2.0, enabled=True),
] + [EQBand("bell", 1000.0, 1.0, 0.0, enabled=False) for _ in range(10)]


class MultibandEQ:
    """16 bands in series -- the shape of the eventual graph-library node:
    prepare() off the audio thread, process_sample() on it, reset() clears
    all state (used e.g. before re-enabling a band, see BypassableBand).
    """

    def __init__(self, bands):
        assert len(bands) == 16, "Pro-Q-class EQ is fixed at 16 bands"
        self.bands = bands

    def prepare(self, fs):
        for b in self.bands:
            b.prepare(fs)

    def reset(self):
        for b in self.bands:
            b.reset()

    def process_sample(self, x):
        y = x
        for b in self.bands:
            if b.enabled:
                y = b.process_sample(y)
        return y

    def process(self, x):
        y = np.empty_like(x, dtype=np.float64)
        for i in range(len(x)):
            y[i] = self.process_sample(x[i])
        return y


def verify_part3_curve():
    fs = sr
    eq = MultibandEQ(DEFAULT_16_BANDS)
    eq.prepare(fs)

    fig, ax = plt.subplots(figsize=(10, 6))

    # Combined response: impulse through the whole (enabled-only) chain.
    n = 32768
    impulse = np.zeros(n)
    impulse[0] = 1.0
    eq.reset()
    ir = eq.process(impulse)
    H = np.fft.rfft(ir)
    freqs = np.fft.rfftfreq(n, d=1 / fs)
    mag_combined = 20 * np.log10(np.abs(H) + 1e-15)
    ax.semilogx(freqs[1:], mag_combined[1:], color="black", lw=2.5, label="combined")

    # Individual responses, each band in isolation.
    for b in DEFAULT_16_BANDS:
        if not b.enabled:
            continue
        solo = EQBand(b.band_type, b.f0, b.Q, b.gain_db, b.slope, True)
        solo.prepare(fs)
        ir_solo = np.empty(n)
        for i in range(n):
            ir_solo[i] = solo.process_sample(impulse[i])
        H_solo = np.fft.rfft(ir_solo)
        mag_solo = 20 * np.log10(np.abs(H_solo) + 1e-15)
        ax.semilogx(freqs[1:], mag_solo[1:], lw=1, ls="--",
                     label=f"{b.band_type}@{b.f0:.0f}Hz {b.gain_db:+.1f}dB")

    ax.set_title("16-band EQ: realistic curve, combined vs. individual bands")
    ax.set_xlabel("Hz"); ax.set_ylabel("dB")
    ax.set_xlim(20, fs / 2); ax.set_ylim(-30, 15)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=7, ncol=2)
    plt.tight_layout()
    plt.savefig(f"{OUT}/part3_curve.png", dpi=130)
    plt.close(fig)

    # Sanity assertions: HP band should be well down at 20Hz, shelf/bell
    # peaks should land near their configured gains at their own f0.
    all_pass = True

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[part3] {status}: {name} -- {detail}")

    at_20 = db_at(freqs, mag_combined, 20.0)
    # 20 Hz is less than an octave below the 30 Hz cutoff, and a Butterworth
    # magnitude hasn't reached its asymptotic 24 dB/oct slope that close in
    # -- so "well below 0 dB" is the right bar, not the full -24 dB/oct*log2
    # ideal-asymptote number.
    check("combined curve: HP attenuates below cutoff", at_20 < -8.0,
          f"measured {at_20:.2f} dB at 20 Hz (want clearly attenuated, HP is 24 dB/oct @ 30 Hz)")

    at_10k = db_at(freqs, mag_combined, 10000.0)
    check("combined curve: high-shelf cut audible near 10 kHz",
          at_10k < 1.0, f"measured {at_10k:.2f} dB at 10 kHz")

    return all_pass


class BypassableBand:
    """Wraps one EQBand with a 10ms linear dry/wet crossfade so toggling
    enabled/disabled mid-signal doesn't click.

    Key rule (per spec): on DISABLE we keep running the wet path while `a`
    ramps 1->0 -- the filter doesn't go silent, only its contribution to the
    output blend shrinks, so there's nothing abrupt for the ear to catch.
    On ENABLE we reset() the filter's state FIRST, then ramp `a` 0->1 --
    otherwise the filter would resume from stale state left over from
    whatever was passing through it before it was switched off, producing a
    transient of its own.
    """

    def __init__(self, band, fs, fade_ms=10.0):
        self.band = band
        self.fade_len = max(1, int(round(fs * fade_ms / 1000)))
        self.step = 1.0 / self.fade_len
        self.a = 1.0 if band.enabled else 0.0
        self.target = self.a

    def set_enabled(self, enabled):
        if enabled and self.target == 0.0:
            self.band.reset()
        self.target = 1.0 if enabled else 0.0

    def process_sample(self, x):
        wet = self.band.process_sample(x)  # always compute -- see docstring
        if self.a < self.target:
            self.a = min(self.target, self.a + self.step)
        elif self.a > self.target:
            self.a = max(self.target, self.a - self.step)
        return (1 - self.a) * x + self.a * wet


class HardToggleBand:
    """Contrast case: instantaneous dry/wet switch, no crossfade at all."""

    def __init__(self, band):
        self.band = band
        self.enabled = band.enabled

    def set_enabled(self, enabled):
        if enabled and not self.enabled:
            self.band.reset()
        self.enabled = enabled

    def process_sample(self, x):
        wet = self.band.process_sample(x)
        return wet if self.enabled else x


def make_pink_noise(n, seed=0):
    """Cheap pink noise: white noise shaped 1/sqrt(f) in the frequency
    domain. Good enough as a broadband test signal -- we're checking for
    clicks, not spectral purity.
    """
    rng = np.random.default_rng(seed)
    white = rng.standard_normal(n)
    X = np.fft.rfft(white)
    freqs = np.fft.rfftfreq(n, d=1.0)
    scale = np.ones_like(freqs)
    scale[1:] = 1.0 / np.sqrt(freqs[1:])
    X *= scale
    pink = np.fft.irfft(X, n)
    return pink / (np.max(np.abs(pink)) + 1e-9) * 0.3


def run_bypass_demo():
    fs = sr
    dur = 3.0
    n = int(fs * dur)
    x = make_pink_noise(n, seed=1)

    # A resonant, clearly-audible band: +12 dB bell, Q=4.
    band_soft = EQBand("bell", 1000.0, 4.0, 12.0, enabled=False)
    band_soft.prepare(fs)
    band_hard = EQBand("bell", 1000.0, 4.0, 12.0, enabled=False)
    band_hard.prepare(fs)

    soft = BypassableBand(band_soft, fs, fade_ms=10.0)
    hard = HardToggleBand(band_hard)

    # Toggle schedule: on at 1.0s, off at 2.0s.
    t_on = int(1.0 * fs)
    t_off = int(2.0 * fs)

    y_soft = np.empty(n)
    y_hard = np.empty(n)
    for i in range(n):
        if i == t_on:
            soft.set_enabled(True)
            hard.set_enabled(True)
        if i == t_off:
            soft.set_enabled(False)
            hard.set_enabled(False)
        y_soft[i] = soft.process_sample(x[i])
        y_hard[i] = hard.process_sample(x[i])

    wavfile.write(f"{OUT}/part3_bypass_soft.wav", fs,
                   (np.clip(y_soft, -1, 1) * 32767).astype(np.int16))
    wavfile.write(f"{OUT}/part3_bypass_hard.wav", fs,
                   (np.clip(y_hard, -1, 1) * 32767).astype(np.int16))

    # Click metric: the single-sample jump exactly AT the toggle boundary
    # (y[t] - y[t-1]), vs. the background typical jump (median |diff| well
    # away from any toggle). A window-max metric over many ms would get
    # swamped by ordinary noise jitter (which is just as large as a real
    # click for broadband noise) -- the click is specifically the boundary
    # discontinuity, so measure exactly that sample pair.
    def boundary_jump(y, t):
        return abs(y[t] - y[t - 1])

    def background_jump(y, exclude, window=int(0.05 * fs)):
        mask = np.ones(len(y) - 1, dtype=bool)
        for t in exclude:
            lo, hi = max(0, t - window), t + window
            mask[lo:hi] = False
        return np.median(np.abs(np.diff(y))[mask])

    bg = background_jump(y_soft, [t_on, t_off])
    soft_on = boundary_jump(y_soft, t_on)
    soft_off = boundary_jump(y_soft, t_off)
    hard_on = boundary_jump(y_hard, t_on)
    hard_off = boundary_jump(y_hard, t_off)

    print(f"[part3] background sample-to-sample jump (typical) = {bg:.6f}")
    print(f"[part3] soft crossfade: jump@on={soft_on:.6f}, jump@off={soft_off:.6f}")
    print(f"[part3] hard toggle:    jump@on={hard_on:.6f}, jump@off={hard_off:.6f}")

    all_pass = True

    def check(name, cond, detail):
        nonlocal all_pass
        status = "PASS" if cond else "FAIL"
        if not cond:
            all_pass = False
        print(f"[part3] {status}: {name} -- {detail}")

    # Soft crossfade should stay close to the background noise floor's own
    # jitter (no discontinuity beyond ordinary sample-to-sample variation).
    check("soft crossfade: no click at enable", soft_on < 15 * bg,
          f"{soft_on:.6f} vs background {bg:.6f}")
    check("soft crossfade: no click at disable", soft_off < 15 * bg,
          f"{soft_off:.6f} vs background {bg:.6f}")

    # Hard toggle should show a visibly larger jump than the soft version at
    # DISABLE -- that's the click, and it's unambiguous: right before, the
    # output is the fully-filtered signal; right after, it's raw dry, with
    # no blend at all.
    check("hard toggle: clearly clicks vs soft at disable", hard_off > 3 * soft_off,
          f"hard={hard_off:.6f} vs soft={soft_off:.6f}")

    # ENABLE is a genuinely different case, not a bug: both soft and hard
    # reset() the filter right before turning it on, so the very first wet
    # sample comes from a cold (all-zero) state. For a band type with m0=1
    # (bell, notch, highpass -- direct passthrough term dominates before the
    # filter's internal state has anything to contribute), that first wet
    # sample is itself very close to the dry input, so hard-enable does NOT
    # produce a large single-sample amplitude jump the way hard-disable does.
    # What hard-enable actually does wrong is coarser: the filter's full
    # resonance appears with no 10ms ramp-in, i.e. an abrupt timbral onset
    # over the following milliseconds as the state rings up, not a one
    # sample glitch. Confirm that's really what's happening (hard_on stays
    # near the background floor, not a click) rather than asserting a click
    # that isn't there for this band type.
    check("hard toggle @ enable: no single-sample click (m0=1 passthrough "
          "masks it -- the real cost is the missing 10ms ramp-in, not a jump)",
          hard_on < 15 * bg, f"{hard_on:.6f} vs background {bg:.6f}")

    # Plot envelopes around both toggle instants.
    fig, axs = plt.subplots(2, 1, figsize=(11, 7), sharex=True)
    t_axis = np.arange(n) / fs
    axs[0].plot(t_axis, y_soft, lw=0.4, color="seagreen")
    axs[0].set_title("Soft: 10ms linear crossfade bypass (+12dB Q=4 bell @ 1kHz, "
                      "toggled on@1.0s / off@2.0s) -- no discontinuity")
    axs[0].set_ylabel("amplitude")
    axs[0].axvline(1.0, color="gray", ls=":", lw=0.8)
    axs[0].axvline(2.0, color="gray", ls=":", lw=0.8)

    axs[1].plot(t_axis, y_hard, lw=0.4, color="crimson")
    axs[1].set_title("Hard: instantaneous toggle, no fade -- click visible at each edge")
    axs[1].set_ylabel("amplitude")
    axs[1].set_xlabel("s")
    axs[1].axvline(1.0, color="gray", ls=":", lw=0.8)
    axs[1].axvline(2.0, color="gray", ls=":", lw=0.8)

    plt.tight_layout()
    plt.savefig(f"{OUT}/part3_bypass_envelope.png", dpi=130)
    plt.close(fig)

    return all_pass


# ============================================================
# main
# ============================================================

if __name__ == "__main__":
    print("=== Part 1: 8 SVF band types ===")
    p1 = verify_part1()

    print("\n=== Part 2: variable-slope Butterworth cascades ===")
    p2a, _ = verify_part2_slopes()
    p2b = verify_part2_q_knob()

    print("\n=== Part 3: 16-band cascade + smoothed bypass ===")
    p3a = verify_part3_curve()
    p3b = run_bypass_demo()

    print(f"\nWrote plots and WAVs to {OUT}/")

    all_pass = p1 and p2a and p2b and p3a and p3b
    print(f"\n{'ALL PASS' if all_pass else 'SOME FAILURES'} "
          f"(part1={p1}, part2_slopes={p2a}, part2_qknob={p2b}, "
          f"part3_curve={p3a}, part3_bypass={p3b})")
    assert all_pass, "one or more verifications failed -- see FAIL lines above"
