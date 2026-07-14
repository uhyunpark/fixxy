"""Measure bilinear-transform frequency cramping in our TPT-SVF EQ filters,
vs. their analog prototypes.

Background (docs/ZLEqualizer-references-guide.md, sections 1-3): the
bilinear transform (BLT) maps the whole analog s-plane into a finite z-plane
strip, which warps the frequency axis. Prewarping (g = tan(pi*f0/fs)) fixes
the mapping to be EXACT at one frequency (f0) -- but everywhere else, the
digital filter's response shape is squeezed relative to the analog
prototype, more and more severely as f -> Nyquist. BLT also forces a zero at
z=-1 (Nyquist), which the analog lowpass prototype simply doesn't have.

Measurement method (no fixes, no matched-filter design -- diagnosis only):

Both our digital SVF and the analog prototype it approximates share the same
(k, m0, m1, m2) mixing recipe (see svf_coeffs() in proto/multiband_eq.py):

    H(s) = m0 + m1*(s/D) + m2*(1/D),   D(s) = s^2 + k*s + 1

This is a normalized 2nd-order section where s=j*1 is "the cutoff". The two
curves we compare are this SAME formula evaluated at two different frequency
axes:

  - analog reference:  s = j*W,        W = f / f0                (true ratio)
  - digital actual:     s = j*W_warp,   W_warp = tan(pi*f/fs) / tan(pi*f0/fs)

W_warp is the exact frequency-axis remapping the bilinear transform performs
(this is what "prewarping" buys you: W_warp(f0) = 1 exactly, matching W(f0)
= 1 -- but away from f0 the two axes diverge, especially as f -> fs/2 where
tan(pi*f/fs) -> infinity while W stays finite). The two curves are evaluated
through the IDENTICAL (k, m0, m1, m2) mix, so any difference between them is
attributable ENTIRELY to this axis warp -- i.e. it IS the cramping, isolated
from everything else.

We additionally cross-check "digital actual" against the real SVFSection
(impulse response -> FFT), copied verbatim from proto/multiband_eq.py, to
confirm the closed-form W_warp substitution really does equal what the
filter does -- this is the "exact for bilinear" claim the spec asks us to
verify, not just assume.
"""
import numpy as np
import matplotlib.pyplot as plt
import os

OUT = "proto/out"
os.makedirs(OUT, exist_ok=True)


# ============================================================
# SVF kernel -- copied from proto/multiband_eq.py (not imported: this is a
# standalone diagnostic script and we only need the pieces below).
# ============================================================

def svf_coeffs(fs, f0, dBgain, Q, band_type):
    """Same recipe as multiband_eq.svf_coeffs. Returns (g, k, a1, a2, a3,
    m0, m1, m2). Only bell / high_shelf / lowpass are needed here (the
    three types this measurement covers), but kept general.
    """
    A = 10 ** (dBgain / 40)
    if band_type == "bell":
        g = np.tan(np.pi * f0 / fs)
        k = 1 / (Q * A)
        m0, m1, m2 = 1.0, k * (A * A - 1), 0.0
    elif band_type == "high_shelf":
        g = np.tan(np.pi * f0 / fs) * np.sqrt(A)
        k = 1 / Q
        m0, m1, m2 = A * A, k * (1 - A) * A, 1 - A * A
    elif band_type == "lowpass":
        g = np.tan(np.pi * f0 / fs)
        k = 1 / Q
        m0, m1, m2 = 0.0, 0.0, 1.0
    else:
        raise ValueError(f"unknown band_type {band_type}")

    a1 = 1 / (1 + g * (g + k))
    a2 = g * a1
    a3 = g * a2
    return g, k, a1, a2, a3, m0, m1, m2


class SVFSection:
    """TPT SVF, copied verbatim from multiband_eq.py -- used only for the
    cross-check that the closed-form warp substitution matches the real
    filter's measured impulse response.
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


def measure_response_fft(fs, f0, dBgain, Q, band_type, n=32768):
    """Real digital filter, impulse -> FFT. Ground truth for the
    cross-check against the closed-form warp substitution."""
    sec = SVFSection()
    sec.prepare(fs, f0, dBgain, Q, band_type)
    impulse = np.zeros(n)
    impulse[0] = 1.0
    ir = sec.process(impulse)
    H = np.fft.rfft(ir)
    freqs = np.fft.rfftfreq(n, d=1 / fs)
    mag_db = 20 * np.log10(np.abs(H) + 1e-15)
    return freqs, mag_db


# ============================================================
# Analog prototype and digital-actual (closed form), sharing (k, m0, m1, m2)
# ============================================================

def mix_mag_db(W, k, m0, m1, m2):
    """|m0 + m1*(s/D) + m2*(1/D)| in dB, s = j*W, D = s^2 + k*s + 1.
    W is a normalized frequency ratio -- TRUE (f/f0) for the analog
    prototype, WARPED (tan-ratio) for the digital-actual curve. This
    function is identical either way; only the W fed in differs.
    """
    s = 1j * W
    D = s**2 + k * s + 1
    H = m0 + m1 * (s / D) + m2 * (1 / D)
    return 20 * np.log10(np.abs(H) + 1e-15)


def analog_and_digital_curves(fs, f0, dBgain, Q, band_type, f_max=None, n_pts=4000):
    """Evaluate the SAME (k,m0,m1,m2) mix on the true axis (analog
    reference) and the warped axis (digital actual, closed form -- exact
    for bilinear). Returns freqs, mag_analog_db, mag_digital_db.

    Subtlety: g is NOT always tan(pi*f0/fs). Shelf/tilt types scale it by
    sqrt(A) (or 1/sqrt(A)) -- this is the well-known "shelf pivot shifts
    with gain" design choice (see docs/ZLEqualizer-references-guide.md
    section 2-3: "shelf = LP/HP shifted by gain"). The digital filter's
    exact frequency-warp identity is W_warp(f) = tan(pi*f/fs) / g using the
    ACTUAL g (whatever it is), not tan(pi*f0/fs) -- that's what the real
    SVF's coefficients (a1,a2,a3) are built from. To keep the comparison
    apples-to-apples (isolating the tan-warp itself, not conflating it with
    the deliberate gain-pivot shift), the analog reference must use the
    SAME effective pivot: f0_eff = f0 * (g / tan(pi*f0/fs)), so W_true(f) =
    f / f0_eff. For bell/lowpass/etc (g = tan(pi*f0/fs) exactly, no extra
    scale) this reduces to the naive W_true = f/f0 as expected.
    """
    if f_max is None:
        f_max = fs / 2 * 0.9999  # avoid tan() blowing up exactly at Nyquist
    g, k, _, _, _, m0, m1, m2 = svf_coeffs(fs, f0, dBgain, Q, band_type)
    scale = g / np.tan(np.pi * f0 / fs)  # 1 for bell/lowpass/etc, sqrt(A)-ish for shelf/tilt
    f0_eff = f0 * scale

    freqs = np.linspace(1.0, f_max, n_pts)
    W_true = freqs / f0_eff
    W_warp = np.tan(np.pi * freqs / fs) / g

    mag_analog = mix_mag_db(W_true, k, m0, m1, m2)
    mag_digital = mix_mag_db(W_warp, k, m0, m1, m2)
    return freqs, mag_analog, mag_digital


def db_at(freqs, mag_db, f_target):
    idx = np.argmin(np.abs(freqs - f_target))
    return mag_db[idx]


# ============================================================
# Cross-check: closed-form digital-actual == real SVF impulse response
# ============================================================

def crosscheck_closed_form_vs_real_filter():
    fs = 44100.0
    cases = [("bell", 10000.0, 6.0, 2.0), ("high_shelf", 8000.0, 6.0, 0.7071),
              ("lowpass", 15000.0, 0.0, 0.7071)]
    print("=== Cross-check: closed-form warp substitution vs real SVFSection ===")
    all_ok = True
    for band_type, f0, gain, Q in cases:
        # Dense sampling matters here: the lowpass digital curve dives
        # steeply toward -inf right at Nyquist (the BLT z=-1 zero), so a
        # coarse closed-form grid interpolated against the FFT's own bins
        # introduces spurious sub-dB "error" purely from linear-interp
        # undershoot on a steep curve -- not a real formula mismatch.
        freqs_cf, _, mag_digital_cf = analog_and_digital_curves(fs, f0, gain, Q, band_type,
                                                                  f_max=fs / 2 * 0.999, n_pts=40000)
        freqs_fft, mag_fft = measure_response_fft(fs, f0, gain, Q, band_type, n=65536)
        # compare at matched frequency points, skip DC and very top bin.
        # Also floor both curves before differencing: right at Nyquist the
        # lowpass digital response dives towards -inf dB and a few
        # 1e-13-scale float differences there get blown up into a fake
        # "error" of a fraction of a dB despite both curves agreeing the
        # signal is >90 dB down -- floor at -90 dB so what's being compared
        # is the audible range, not FFT noise floor drift.
        mask = (freqs_fft > 20) & (freqs_fft < fs / 2 * 0.999)
        mag_cf_interp = np.interp(freqs_fft[mask], freqs_cf, mag_digital_cf)
        FLOOR_DB = -90.0
        err = np.max(np.abs(np.maximum(mag_cf_interp, FLOOR_DB) - np.maximum(mag_fft[mask], FLOOR_DB)))
        ok = err < 0.05
        all_ok &= ok
        print(f"  {band_type:10s} f0={f0:>6.0f}  max|closed-form - real filter| = {err:.5f} dB "
              f"[{'OK' if ok else 'MISMATCH'}]")
    if not all_ok:
        raise RuntimeError("closed-form digital-actual curve does not match the real SVF -- "
                            "the 'exact for bilinear' assumption is wrong, fix before trusting the rest")
    print("  closed-form W_warp substitution reproduces the real filter to <0.05 dB -- confirmed exact.\n")


# ============================================================
# Section 1+2: analog vs digital-actual, per family, + quantified table
# ============================================================

def measure_family(configs, fs=44100.0):
    """configs: list of (band_type, f0, gain_db, Q, label). Returns list of
    dicts with freqs/mag_analog/mag_digital/delta plus the 3 scalar metrics.
    """
    rows = []
    for band_type, f0, gain, Q, label in configs:
        freqs, mag_a, mag_d = analog_and_digital_curves(fs, f0, gain, Q, band_type)
        delta = mag_d - mag_a

        below_20k = freqs < 20000.0
        max_abs_delta = np.max(np.abs(delta[below_20k])) if np.any(below_20k) else np.nan
        delta_at_f0 = delta[np.argmin(np.abs(freqs - f0))]
        delta_at_09nyq = delta[np.argmin(np.abs(freqs - 0.9 * fs / 2))]

        rows.append(dict(band_type=band_type, f0=f0, gain=gain, Q=Q, label=label, fs=fs,
                          freqs=freqs, mag_a=mag_a, mag_d=mag_d, delta=delta,
                          max_abs_delta=max_abs_delta, delta_at_f0=delta_at_f0,
                          delta_at_09nyq=delta_at_09nyq))
    return rows


def plot_family(rows, title, fname, mag_ylim=(-8, 12)):
    ncols = len(rows)
    fig, axs = plt.subplots(2, ncols, figsize=(4.5 * ncols, 7), sharex="col")
    if ncols == 1:
        axs = axs.reshape(2, 1)

    for col, r in enumerate(rows):
        ax_mag, ax_delta = axs[0, col], axs[1, col]
        ax_mag.semilogx(r["freqs"], r["mag_a"], label="analog prototype", color="tab:blue")
        ax_mag.semilogx(r["freqs"], r["mag_d"], label="digital (BLT, prewarped)",
                         color="tab:red", ls="--")
        ax_mag.axvline(r["f0"], color="gray", lw=0.5, ls=":")
        ax_mag.set_title(r["label"])
        ax_mag.set_xlim(20, r["fs"] / 2)
        ax_mag.set_ylim(*mag_ylim)
        ax_mag.grid(True, which="both", alpha=0.3)
        if col == 0:
            ax_mag.set_ylabel("dB")
            ax_mag.legend(fontsize=8)

        ax_delta.semilogx(r["freqs"], r["delta"], color="tab:purple")
        ax_delta.axhline(0, color="black", lw=0.5)
        ax_delta.axvline(r["f0"], color="gray", lw=0.5, ls=":")
        ax_delta.axvline(0.9 * r["fs"] / 2, color="orange", lw=0.5, ls=":")
        ax_delta.set_xlim(20, r["fs"] / 2)
        ax_delta.set_xlabel("Hz")
        ax_delta.grid(True, which="both", alpha=0.3)
        if col == 0:
            ax_delta.set_ylabel("delta = digital - analog (dB)\n(= cramping)")

    fig.suptitle(title)
    plt.tight_layout()
    plt.savefig(f"{OUT}/{fname}", dpi=130)
    plt.close(fig)


def print_table(all_rows):
    print(f"{'config':<32s} {'fs':>7s} {'max|delta|<20k':>15s} {'delta@f0':>10s} {'delta@0.9Nyq':>13s}")
    print("-" * 82)
    for r in all_rows:
        print(f"{r['label']:<32s} {r['fs']:>7.0f} {r['max_abs_delta']:>15.3f} "
              f"{r['delta_at_f0']:>10.3f} {r['delta_at_09nyq']:>13.3f}")


# ============================================================
# Section 3: sample-rate dependence
# ============================================================

def measure_vs_fs(band_type, f0, gain, Q, fs_list):
    fig, ax = plt.subplots(figsize=(9, 6))
    rows = []
    for fs in fs_list:
        freqs, mag_a, mag_d = analog_and_digital_curves(fs, f0, gain, Q, band_type)
        delta = mag_d - mag_a
        below_20k = freqs < min(20000.0, fs / 2 * 0.999)
        max_abs_delta = np.max(np.abs(delta[below_20k]))
        ax.semilogx(freqs, delta, label=f"fs={fs/1000:.0f}k (max |delta|<20k = {max_abs_delta:.2f} dB)")
        rows.append((fs, max_abs_delta))

    ax.axhline(0, color="black", lw=0.5)
    ax.axvline(f0, color="gray", lw=0.5, ls=":")
    ax.axvline(20000, color="orange", lw=0.5, ls=":")
    ax.set_xlim(20, max(fs_list) / 2)
    ax.set_xlabel("Hz"); ax.set_ylabel("delta = digital - analog (dB)")
    ax.set_title(f"Cramping shrinks with fs: bell +6dB Q={Q}, f0={f0:.0f} Hz "
                 f"(worst config from the bell family)")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=9)
    plt.tight_layout()
    plt.savefig(f"{OUT}/cramping_vs_fs.png", dpi=130)
    plt.close(fig)

    print(f"\n=== Sample-rate dependence: bell +6dB Q={Q} f0={f0:.0f} Hz ===")
    for fs, mad in rows:
        print(f"  fs={fs:>7.0f}  max|delta| below 20kHz = {mad:.4f} dB")
    return rows


# ============================================================
# Section 4: audibility-context zoom plot
# ============================================================

def plot_zoom():
    fs = 44100.0
    f0, gain, Q = 10000.0, 6.0, 2.0
    freqs, mag_a, mag_d = analog_and_digital_curves(fs, f0, gain, Q, "bell",
                                                       f_max=fs / 2 * 0.999, n_pts=8000)
    fig, ax = plt.subplots(figsize=(10, 6))
    ax.plot(freqs, mag_a, label="analog prototype", color="tab:blue", lw=2)
    ax.plot(freqs, mag_d, label="digital (BLT, prewarped)", color="tab:red", lw=2, ls="--")
    ax.axvline(f0, color="gray", lw=0.7, ls=":", label="f0 = 10 kHz")
    ax.set_xlim(5000, 22050)
    ax.set_ylim(-2, 8)
    ax.set_xlabel("Hz (linear)"); ax.set_ylabel("dB")
    ax.set_title("Cramping made visible: 10kHz bell +6dB Q=2 @ 44.1kHz\n"
                  "linear freq axis, zoomed 5k-22k -- digital peak visibly narrower/pinched")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=9)
    plt.tight_layout()
    plt.savefig(f"{OUT}/cramping_zoom.png", dpi=130)
    plt.close(fig)


# ============================================================
# main
# ============================================================

if __name__ == "__main__":
    crosscheck_closed_form_vs_real_filter()

    fs = 44100.0
    all_rows = []

    print("=== 1. Analog reference vs digital-actual, per family ===\n")

    bell_configs = [("bell", f0, 6.0, 2.0, f"bell +6dB Q=2 f0={f0/1000:g}k")
                     for f0 in (1000.0, 5000.0, 10000.0, 15000.0)]
    bell_rows = measure_family(bell_configs, fs)
    plot_family(bell_rows, "Bell +6dB Q=2 -- analog prototype vs digital (BLT)",
                "cramping_bell.png", mag_ylim=(-2, 8))
    all_rows += bell_rows

    shelf_configs = [("high_shelf", f0, 6.0, 0.7071, f"high_shelf +6dB Q=0.707 f0={f0/1000:g}k")
                      for f0 in (5000.0, 8000.0, 12000.0)]
    shelf_rows = measure_family(shelf_configs, fs)
    plot_family(shelf_rows, "High-shelf +6dB Q=0.707 -- analog prototype vs digital (BLT)",
                "cramping_shelf.png", mag_ylim=(-2, 8))
    all_rows += shelf_rows

    lp_configs = [("lowpass", f0, 0.0, 0.7071, f"lowpass Q=0.707 f0={f0/1000:g}k")
                  for f0 in (10000.0, 15000.0, 18000.0)]
    lp_rows = measure_family(lp_configs, fs)
    plot_family(lp_rows, "Lowpass Q=0.707 -- analog prototype vs digital (BLT)",
                "cramping_lp.png", mag_ylim=(-40, 5))
    all_rows += lp_rows

    print("=== 2. Quantified table ===\n")
    print_table(all_rows)
    print()
    print("Qualitative signatures:")
    print("  - bell: as f0 -> Nyquist, the digital peak is measurably NARROWER than the")
    print("    analog prototype at the same true Q -- the whole bump gets pinched toward f0.")
    print("  - high_shelf: the post-shoulder plateau bows away from the analog's flat")
    print("    asymptote as f -> Nyquist (BLT can't hold a flat shelf all the way up).")
    print("  - lowpass: BLT forces a zero at z=-1 (Nyquist) that the analog 2nd-order")
    print("    lowpass prototype does not have -- digital goes to -inf dB exactly at")
    print("    Nyquist while the analog curve stays finite; delta blows up near the edge.")

    # 3. sample-rate dependence -- worst bell config (highest f0 relative to fs/2)
    fs_list = [44100.0, 48000.0, 96000.0, 192000.0]
    measure_vs_fs("bell", 15000.0, 6.0, 2.0, fs_list)

    # 4. audibility-context zoom
    plot_zoom()

    print(f"\nWrote plots to {OUT}/: cramping_bell.png, cramping_shelf.png, cramping_lp.png, "
          f"cramping_vs_fs.png, cramping_zoom.png")

    # ---- verdict ----
    print("\n=== Verdict: where does cramping plausibly matter musically? ===")
    print("(threshold: max|delta| below 20 kHz, at fs=44100 unless noted)\n")
    exceed_1db = []
    exceed_half_db = []
    for r in all_rows:
        if r["max_abs_delta"] >= 1.0:
            exceed_1db.append(r)
        elif r["max_abs_delta"] >= 0.5:
            exceed_half_db.append(r)

    print(f"Exceeds 1 dB below 20 kHz ({len(exceed_1db)} configs):")
    for r in sorted(exceed_1db, key=lambda x: -x["max_abs_delta"]):
        print(f"  {r['label']:<32s} max|delta|={r['max_abs_delta']:.2f} dB")

    print(f"\nExceeds 0.5 dB but < 1 dB below 20 kHz ({len(exceed_half_db)} configs):")
    for r in sorted(exceed_half_db, key=lambda x: -x["max_abs_delta"]):
        print(f"  {r['label']:<32s} max|delta|={r['max_abs_delta']:.2f} dB")

    under_half = [r for r in all_rows if r["max_abs_delta"] < 0.5]
    print(f"\nUnder 0.5 dB below 20 kHz ({len(under_half)} configs, likely inaudible):")
    for r in sorted(under_half, key=lambda x: -x["max_abs_delta"]):
        print(f"  {r['label']:<32s} max|delta|={r['max_abs_delta']:.2f} dB")
