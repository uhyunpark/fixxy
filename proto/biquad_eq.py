"""Biquad peaking EQ: RBJ direct-form vs. TPT-SVF (Cytomic) under modulation.

Part 1: RBJ peaking biquad (Direct Form II Transposed), verified against the
        analytic design via scipy.signal.freqz.
Part 2: sweep f0 continuously while recomputing biquad coefficients every
        block, carrying the SAME z1/z2 state across the coefficient change.
        This is the trap: a direct-form filter's internal state is only valid
        for the coefficients it was computed under. Swap coefficients under
        that state and you get a transient -- at high Q / high gain that
        transient is loud enough to hear as a click or a burst of ringing on
        every coefficient update, i.e. "zipper" noise baked into the filter
        itself (as opposed to zipper on a gain multiply, which zipper_gain.py
        already demonstrates).
Part 3: TPT (topology-preserving transform) State Variable Filter in Cytomic's
        form. Its two state variables (ic1eq, ic2eq) are physical integrator
        states, not filter-coefficient-dependent history -- so recomputing
        coefficients every sample and swapping them under the SAME state is
        exactly what this topology is designed for. Verified to match the RBJ
        biquad's static response, then run through the identical sweep to
        show the artifact is gone.
"""
import numpy as np
from scipy.io import wavfile
from scipy.signal import freqz
import matplotlib.pyplot as plt
import os

sr = 44100

OUT = "proto/out"
os.makedirs(OUT, exist_ok=True)


# ============================================================
# PART 1: RBJ peaking biquad (Direct Form II Transposed)
# ============================================================

def rbj_peaking_coeffs(fs, f0, dBgain, Q):
    """RBJ cookbook peaking-EQ coefficients, normalized by a0.

    Returns (b0, b1, b2, a1, a2) with a0 already divided out -- DF2T below
    assumes a0 == 1.
    """
    A = 10 ** (dBgain / 40)
    w0 = 2 * np.pi * f0 / fs
    cosw0 = np.cos(w0)
    sinw0 = np.sin(w0)
    alpha = sinw0 / (2 * Q)

    b0 = 1 + alpha * A
    b1 = -2 * cosw0
    b2 = 1 - alpha * A
    a0 = 1 + alpha / A
    a1 = -2 * cosw0
    a2 = 1 - alpha / A

    return b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0


class DF2TBiquad:
    """Direct Form II Transposed biquad with persistent state (z1, z2).

    DF2T is the standard real-time form: only two state registers, and
    (unlike Direct Form I) it doesn't need extra history for the numerator
    and denominator separately. But the state registers store history that
    is only meaningful under the coefficients that produced it -- that's
    exactly what Part 2 exploits to show the failure mode.
    """

    def __init__(self):
        self.z1 = 0.0
        self.z2 = 0.0

    def set_coeffs(self, b0, b1, b2, a1, a2):
        self.b0, self.b1, self.b2, self.a1, self.a2 = b0, b1, b2, a1, a2

    def process(self, x):
        """Process a block (1D numpy array) in place, sample by sample."""
        y = np.empty_like(x)
        b0, b1, b2, a1, a2 = self.b0, self.b1, self.b2, self.a1, self.a2
        z1, z2 = self.z1, self.z2
        for i in range(len(x)):
            xi = x[i]
            yi = b0 * xi + z1
            z1 = b1 * xi - a1 * yi + z2
            z2 = b2 * xi - a2 * yi
            y[i] = yi
        self.z1, self.z2 = z1, z2
        return y


def verify_part1():
    """Plot RBJ biquad magnitude response and confirm peak/flat/symmetry."""
    fs = sr
    f0 = 1000.0
    Q = 1.0

    fig, ax = plt.subplots(figsize=(8, 5))
    results = {}
    for dBgain in (-12, -6, 0, 6, 12):
        b0, b1, b2, a1, a2 = rbj_peaking_coeffs(fs, f0, dBgain, Q)
        w, h = freqz([b0, b1, b2], [1, a1, a2], worN=4096, fs=fs)
        mag_db = 20 * np.log10(np.abs(h) + 1e-12)
        ax.semilogx(w, mag_db, label=f"{dBgain:+.0f} dB")
        results[dBgain] = (w, mag_db)

    ax.set_title(f"RBJ peaking biquad, f0={f0} Hz, Q={Q}")
    ax.set_xlabel("Hz")
    ax.set_ylabel("dB")
    ax.set_xlim(20, fs / 2)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    plt.tight_layout()
    plt.savefig(f"{OUT}/part1_rbj_response.png", dpi=130)
    plt.close(fig)

    # Numeric checks.
    w, mag0 = results[0]
    peak_flat = np.max(np.abs(mag0))
    print(f"[part1] 0 dB gain: max deviation from flat = {peak_flat:.4f} dB (want ~0)")

    for dBgain in (-12, -6, 6, 12):
        w, mag = results[dBgain]
        idx = np.argmin(np.abs(w - f0))
        peak_at_f0 = mag[idx]
        print(f"[part1] gain={dBgain:+d} dB: measured at f0 = {peak_at_f0:.3f} dB")

    # Symmetry: boost and cut of the same magnitude should mirror around 0 dB.
    _, mag_boost = results[12]
    _, mag_cut = results[-12]
    idx = np.argmin(np.abs(w - f0))
    print(f"[part1] symmetry check at f0: boost={mag_boost[idx]:.3f}, "
          f"cut={mag_cut[idx]:.3f}, sum={mag_boost[idx] + mag_cut[idx]:.4f} (want ~0)")


# ============================================================
# PART 2: sweep f0 through the DF2T biquad -- demonstrate the trap
# ============================================================

def make_sweep_signal(fs, dur, amp=0.3):
    """White noise carrier -- broadband so the swept peak is always excited,
    unlike a sine which only rings when the sweep crosses its own frequency.
    """
    n = int(fs * dur)
    rng = np.random.default_rng(0)
    return rng.uniform(-amp, amp, n).astype(np.float64)


def f0_sweep(n, fs, f_start, f_end):
    """Exponential (musical) sweep from f_start to f_end over n samples."""
    t = np.arange(n) / fs
    dur = n / fs
    k = (f_end / f_start) ** (1 / dur)
    return f_start * k ** t


def run_biquad_sweep(block=64, Q=6.0, dBgain=12.0, dur=2.0):
    """Sweep f0 200 Hz -> 5 kHz, recomputing biquad coeffs every block,
    carrying the same z1/z2 state across each coefficient swap.

    WHAT TO LISTEN FOR: a train of clicks/rasps riding on the swept
    resonance, on top of the noise -- one per coefficient update. High Q
    means the filter's poles are close to the unit circle, so its impulse
    response rings for a while; slamming in new coefficients while that
    ringing (stored in z1/z2) is still live injects energy the new filter
    was never designed to carry, and it decays as an audible transient.
    At worst (Q even higher, gain higher) this can build up faster than it
    decays and blow up.
    """
    n = int(sr * dur)
    x = make_sweep_signal(sr, dur)
    f0_track = f0_sweep(n, sr, 200.0, 5000.0)

    biquad = DF2TBiquad()
    y = np.empty(n)
    for start in range(0, n, block):
        end = min(start + block, n)
        f0 = f0_track[start]  # coefficients frozen for this block
        b0, b1, b2, a1, a2 = rbj_peaking_coeffs(sr, f0, dBgain, Q)
        biquad.set_coeffs(b0, b1, b2, a1, a2)
        y[start:end] = biquad.process(x[start:end])

    # Guard against the filter actually blowing up before we try to write a WAV.
    peak = np.max(np.abs(y))
    print(f"[part2] biquad sweep peak |y| = {peak:.3f}")
    y_safe = np.clip(y, -4.0, 4.0)

    wavfile.write(f"{OUT}/part2_biquad_sweep.wav", sr,
                   (np.clip(y_safe, -1, 1) * 32767).astype(np.int16))
    return y


# ============================================================
# PART 3: TPT-SVF peaking bell (Cytomic form) -- the fix
# ============================================================

class TPTSVFPeak:
    """Topology-Preserving Transform State Variable Filter, peaking-bell mode.

    State (ic1eq, ic2eq) represents the physical state of two trapezoidally-
    integrated integrators, not a filter-coefficient-dependent delay line.
    Coefficients (g, k, a1, a2, a3) only scale how state is *read* each
    sample -- they don't own the state -- so they can change every sample
    without corrupting what's stored. That's the whole point of the "TPT"
    construction: it keeps the digital structure topologically identical to
    the analog RC/op-amp circuit it emulates, and analog component values
    can be swept continuously without the circuit's charge going anywhere
    it shouldn't.
    """

    def __init__(self):
        self.ic1eq = np.float64(0.0)
        self.ic2eq = np.float64(0.0)

    def set_coeffs(self, fs, f0, dBgain, Q):
        A = 10 ** (dBgain / 40)
        g = np.tan(np.pi * f0 / fs)
        k = 1 / (Q * A)
        a1 = 1 / (1 + g * (g + k))
        a2 = g * a1
        a3 = g * a2
        m1 = k * (A * A - 1)
        self.a1, self.a2, self.a3 = np.float64(a1), np.float64(a2), np.float64(a3)
        self.m1 = np.float64(m1)
        # m0 = 1, m2 = 0 for the peaking bell -- baked directly into process().

    def process(self, x):
        y = np.empty_like(x)
        a1, a2, a3, m1 = self.a1, self.a2, self.a3, self.m1
        ic1eq, ic2eq = self.ic1eq, self.ic2eq
        for i in range(len(x)):
            v0 = np.float64(x[i])
            v3 = v0 - ic2eq
            v1 = a1 * ic1eq + a2 * v3
            v2 = ic2eq + a2 * ic1eq + a3 * v3
            ic1eq = 2 * v1 - ic1eq
            ic2eq = 2 * v2 - ic2eq
            y[i] = v0 + m1 * v1  # m0=1, m2=0
        self.ic1eq, self.ic2eq = ic1eq, ic2eq
        return y

    def process_modulated(self, x, fs, f0_track, dBgain, Q):
        """Recompute coefficients every sample from f0_track, then step.
        This is the stress test: coefficients change under live state.
        """
        y = np.empty_like(x)
        ic1eq, ic2eq = self.ic1eq, self.ic2eq
        A = 10 ** (dBgain / 40)
        k = 1 / (Q * A)
        for i in range(len(x)):
            g = np.tan(np.pi * f0_track[i] / fs)
            a1 = 1 / (1 + g * (g + k))
            a2 = g * a1
            a3 = g * a2
            m1 = k * (A * A - 1)

            v0 = np.float64(x[i])
            v3 = v0 - ic2eq
            v1 = a1 * ic1eq + a2 * v3
            v2 = ic2eq + a2 * ic1eq + a3 * v3
            ic1eq = 2 * v1 - ic1eq
            ic2eq = 2 * v2 - ic2eq
            y[i] = v0 + m1 * v1
        self.ic1eq, self.ic2eq = ic1eq, ic2eq
        return y


def measure_svf_response(fs, f0, dBgain, Q, worN=4096):
    """Measure the SVF's static magnitude response empirically: excite with
    an impulse (SVF has no closed-form transfer function derivation here --
    we're using the given time-domain recurrence as ground truth), then FFT.
    """
    svf = TPTSVFPeak()
    svf.set_coeffs(fs, f0, dBgain, Q)
    n = worN * 4
    impulse = np.zeros(n)
    impulse[0] = 1.0
    ir = svf.process(impulse)
    H = np.fft.rfft(ir)
    freqs = np.fft.rfftfreq(n, d=1 / fs)
    mag_db = 20 * np.log10(np.abs(H) + 1e-12)
    return freqs, mag_db


def verify_part3a():
    """Overlay RBJ biquad freqz response and SVF impulse-response FFT --
    they should sit on top of each other."""
    fs = sr
    f0 = 1000.0
    Q = 1.0

    fig, ax = plt.subplots(figsize=(8, 5))
    max_err = 0.0
    for dBgain in (-12, -6, 0, 6, 12):
        b0, b1, b2, a1, a2 = rbj_peaking_coeffs(fs, f0, dBgain, Q)
        w, h = freqz([b0, b1, b2], [1, a1, a2], worN=4096, fs=fs)
        mag_biquad = 20 * np.log10(np.abs(h) + 1e-12)

        freqs, mag_svf = measure_svf_response(fs, f0, dBgain, Q, worN=4096)
        mag_svf_interp = np.interp(w, freqs, mag_svf)

        ax.semilogx(w, mag_biquad, label=f"RBJ {dBgain:+.0f} dB", lw=2)
        ax.semilogx(w, mag_svf_interp, "--", label=f"SVF {dBgain:+.0f} dB", lw=1.2)

        # Compare only in the band where both measurements are well-conditioned
        # (avoid DC / Nyquist edge artifacts of the impulse-response FFT).
        band = (w > 50) & (w < fs / 2 - 200)
        err = np.max(np.abs(mag_biquad[band] - mag_svf_interp[band]))
        max_err = max(max_err, err)

    ax.set_title(f"RBJ biquad vs TPT-SVF, f0={f0} Hz, Q={Q} (should overlap)")
    ax.set_xlabel("Hz")
    ax.set_ylabel("dB")
    ax.set_xlim(20, fs / 2)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=7, ncol=2)
    plt.tight_layout()
    plt.savefig(f"{OUT}/part3a_biquad_vs_svf.png", dpi=130)
    plt.close(fig)

    print(f"[part3a] max |RBJ - SVF| magnitude error across all gains = {max_err:.4f} dB")
    return max_err


def run_svf_sweep(Q=6.0, dBgain=12.0, dur=2.0):
    """Same sweep as Part 2, but through the SVF, recomputing coefficients
    every sample. Should be clean: no clicks, no blowup, because the state
    variables don't care what the coefficients were a sample ago.
    """
    n = int(sr * dur)
    x = make_sweep_signal(sr, dur)
    f0_track = f0_sweep(n, sr, 200.0, 5000.0)

    svf = TPTSVFPeak()
    y = svf.process_modulated(x, sr, f0_track, dBgain, Q)

    peak = np.max(np.abs(y))
    print(f"[part3b] SVF sweep peak |y| = {peak:.3f}")

    wavfile.write(f"{OUT}/part3b_svf_sweep.wav", sr,
                   (np.clip(y, -1, 1) * 32767).astype(np.int16))
    return y


def envelope(y, win=256):
    """Simple envelope follower for plotting: moving max of |y|, downsampled.
    Makes clicks/blowups visible as spikes without plotting every sample.
    """
    n_win = len(y) // win
    y_trim = y[:n_win * win].reshape(n_win, win)
    return np.max(np.abs(y_trim), axis=1)


def plot_sweep_comparison(y_biquad, y_svf, dur):
    fig, axs = plt.subplots(2, 1, figsize=(11, 7), sharex=True)
    t_env_b = np.linspace(0, dur, len(envelope(y_biquad)))
    t_env_s = np.linspace(0, dur, len(envelope(y_svf)))

    axs[0].plot(t_env_b, envelope(y_biquad), color="crimson")
    axs[0].set_title("DF2T biquad, f0 swept 200 Hz -> 5 kHz, coeffs recomputed every block "
                      "(Q=6, +12 dB) -- zipper/ringing transients at each update")
    axs[0].set_ylabel("|y| envelope")
    axs[0].grid(alpha=0.3)

    axs[1].plot(t_env_s, envelope(y_svf), color="seagreen")
    axs[1].set_title("TPT-SVF, same sweep, coeffs recomputed every sample -- clean")
    axs[1].set_ylabel("|y| envelope")
    axs[1].set_xlabel("s")
    axs[1].grid(alpha=0.3)

    plt.tight_layout()
    plt.savefig(f"{OUT}/part2_3_sweep_comparison.png", dpi=130)
    plt.close(fig)


# ============================================================
# Optional live playback (guarded -- no hard fail without a device)
# ============================================================

def try_playback(y, fs, label):
    try:
        import sounddevice as sd
        print(f"Playing {label}...")
        sd.play(np.clip(y, -1, 1), fs)
        sd.wait()
    except Exception as e:
        print(f"[playback skipped for {label}: {e}]")


if __name__ == "__main__":
    print("=== Part 1: RBJ peaking biquad, static response ===")
    verify_part1()

    print("\n=== Part 2: DF2T biquad under continuous f0 sweep (the trap) ===")
    y_biquad = run_biquad_sweep()

    print("\n=== Part 3a: TPT-SVF vs RBJ static response (should match) ===")
    max_err = verify_part3a()
    if max_err < 0.5:
        print(f"[part3a] PASS: biquad and SVF static responses match within {max_err:.4f} dB")
    else:
        print(f"[part3a] WARNING: mismatch of {max_err:.4f} dB exceeds tolerance")

    print("\n=== Part 3b: TPT-SVF under continuous f0 sweep (the fix) ===")
    y_svf = run_svf_sweep()

    plot_sweep_comparison(y_biquad, y_svf, dur=2.0)

    print(f"\nWrote plots and WAVs to {OUT}/")

    # Uncomment to listen (guarded -- won't fail without an audio device):
    # try_playback(y_biquad, sr, "biquad sweep")
    # try_playback(y_svf, sr, "svf sweep")
