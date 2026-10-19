"""Verified portable DSP slice for the spectral functional-parity rewrite.

This is OUR approximation, not evidence of soothe3's proprietary algorithm.
It implements the formulas selected in ``docs/soothe3-parity-dsp-spec.md``:

* real-cepstral minimum-phase and symmetric linear-phase FIR synthesis;
* a causal 64-sample direct head plus uniformly partitioned FFT tail;
* history-correct two-bank IR transitions and a direct time-varying oracle;
* Soft/Hard control curves, true per-frequency Detail and all six tilts;
* power-domain stereo linking, M/S focus, curve focus, and union masks.

Run from the repository root with ``proto/.venv/bin/python
proto/spectral_parity.py``.  Numbered assert gates print measured maxima.  The
fixture dumper imports ``run_all_gates`` and refuses to emit unless all gates
pass.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import time

import numpy as np
from scipy import signal
from scipy.io import wavfile
from scipy.ndimage import median_filter


FS = 48_000.0
DESIGN_FFT = 16_384
MINIMUM_TAPS = 4_096
PARTITION = 64
LINEAR_DELAY = 2_048
LINEAR_TAPS = 2 * LINEAR_DELAY + 1
MAX_REDUCTION_DB = 40.0
EPS = 1.0e-12
OUT = Path(__file__).resolve().parent / "out"


def shelf_coordinates(freqs: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """The designed low/high shelf coordinates; DC is explicitly low-only."""
    f = np.asarray(freqs, dtype=np.float64)
    low = 1.0 / (1.0 + (f / 500.0) ** 4)
    high = np.zeros_like(f)
    pos = f > 0.0
    high[pos] = 1.0 / (1.0 + (2000.0 / f[pos]) ** 4)
    return low, high


def detail_profile(freqs: np.ndarray, detail: float, detail_lo: float = 0.0,
                   detail_hi: float = 0.0) -> np.ndarray:
    low, high = shelf_coordinates(freqs)
    return np.clip(detail + detail_lo * low + detail_hi * high, 0.0, 10.0)


def detail_width_oct(detail: np.ndarray) -> np.ndarray:
    return 0.5 * np.power(2.0, -np.asarray(detail) / 2.0)


def detail_strength(detail: np.ndarray) -> np.ndarray:
    return np.power(2.0, (np.asarray(detail) - 5.0) / 10.0)


def detector_tau(freqs: np.ndarray, detail: np.ndarray) -> np.ndarray:
    del freqs  # frequency dependence enters through d(f), kept explicit in API.
    return 0.001 * np.power(2.0, np.asarray(detail) / 2.0)


def time_constants(freqs: np.ndarray, attack: float, release: float,
                   attack_lo: float = 0.0, attack_hi: float = 0.0,
                   release_lo: float = 0.0, release_hi: float = 0.0
                   ) -> tuple[np.ndarray, np.ndarray]:
    """Per-bin attack/release seconds from normalized controls and two tilts."""
    f = np.asarray(freqs, dtype=np.float64)
    low, high = shelf_coordinates(f)
    safe_f = np.maximum(f, 1.0)
    frequency_scale = np.clip(np.sqrt(1000.0 / safe_f), 0.25, 4.0)
    attack_1k = 0.001 * 27.0 ** (attack / 5.0)
    release_1k = 0.005 * 10.0 ** (release / 5.0)
    tau_a = attack_1k * frequency_scale * np.power(
        2.0, attack_lo * low + attack_hi * high)
    tau_r = release_1k * frequency_scale * np.power(
        2.0, release_lo * low + release_hi * high)
    return tau_a, tau_r


def frame_ballistics(target: np.ndarray, previous: np.ndarray, fs: float,
                     hop: int, tau_attack: np.ndarray,
                     tau_release: np.ndarray) -> np.ndarray:
    """Branching one-pole reduction smoothing at the actual detector cadence."""
    target = np.asarray(target, dtype=np.float64)
    previous = np.asarray(previous, dtype=np.float64)
    tau = np.where(target > previous, tau_attack, tau_release)
    alpha = np.exp(-hop / (fs * tau))
    return alpha * previous + (1.0 - alpha) * target


def soft_detector_step(exceed_db: np.ndarray, previous_db: np.ndarray,
                       fs: float, hop: int,
                       tau_detector: np.ndarray) -> np.ndarray:
    """Detail-dependent symmetric one-pole smoothing of Soft exceedance."""
    alpha = np.exp(-hop / (fs * tau_detector))
    return alpha * previous_db + (1.0 - alpha) * exceed_db


def decoupled_ballistics_step(target_db: np.ndarray, y_previous: np.ndarray,
                              release_state: np.ndarray, fs: float, hop: int,
                              tau_attack: np.ndarray, tau_release: np.ndarray
                              ) -> tuple[np.ndarray, np.ndarray]:
    """Existing decoupled attack/release law, vectorized over frequencies.

    Rising reduction demand enters the release state immediately; release is a
    one-pole fall. The attack pole then smooths that state independently.
    """
    a_attack = np.exp(-hop / (fs * tau_attack))
    a_release = np.exp(-hop / (fs * tau_release))
    release = np.maximum(target_db,
                         a_release * (release_state - target_db) + target_db)
    y = a_attack * (y_previous - release) + release
    return y, release


def smooth_cap_and_clamp(envelope_db: np.ndarray, previous_cap_db: float,
                         target_cap_db: float, fs: float, hop: int,
                         tau_seconds: float = 0.020
                         ) -> tuple[np.ndarray, float]:
    """Smooth the Max-cut control and constrain even previously deeper state."""
    alpha = np.exp(-hop / (fs * tau_seconds))
    cap = alpha * previous_cap_db + (1.0 - alpha) * target_cap_db
    return np.minimum(np.asarray(envelope_db, dtype=np.float64), cap), float(cap)


def variable_box_smooth_direct(values: np.ndarray, freqs: np.ndarray,
                               width_oct: np.ndarray) -> np.ndarray:
    """Destination-dependent octave box smoothing with normalized weights."""
    values = np.asarray(values, dtype=np.float64)
    freqs = np.asarray(freqs, dtype=np.float64)
    out = np.empty_like(values)
    for k, (f, width) in enumerate(zip(freqs, width_oct)):
        if f <= 0.0:
            out[k] = values[k]
            continue
        radius = 0.5 * width
        lo = np.searchsorted(freqs, f * 2.0 ** (-radius), side="left")
        hi = np.searchsorted(freqs, f * 2.0 ** radius, side="right")
        weights = np.full(hi - lo, 1.0 / (hi - lo), dtype=np.float64)
        out[k] = np.dot(weights, values[lo:hi])
    return out


def variable_box_smooth(values: np.ndarray, freqs: np.ndarray,
                        width_oct: np.ndarray) -> np.ndarray:
    """Prefix-sum implementation of :func:`variable_box_smooth_direct`."""
    values = np.asarray(values, dtype=np.float64)
    freqs = np.asarray(freqs, dtype=np.float64)
    out = np.empty_like(values)
    prefix = np.concatenate(([0.0], np.cumsum(values)))
    for k, (f, width) in enumerate(zip(freqs, width_oct)):
        if f <= 0.0:
            out[k] = values[k]
            continue
        radius = 0.5 * width
        lo = np.searchsorted(freqs, f * 2.0 ** (-radius), side="left")
        hi = np.searchsorted(freqs, f * 2.0 ** radius, side="right")
        out[k] = (prefix[hi] - prefix[lo]) / (hi - lo)
    return out


def octave_median(amplitude: np.ndarray, freqs: np.ndarray,
                  half_width_oct: float = 0.5) -> np.ndarray:
    """Independent, exact local-octave median background used by Soft mode."""
    amplitude = np.asarray(amplitude, dtype=np.float64)
    out = np.empty_like(amplitude)
    for k, f in enumerate(freqs):
        if f <= 0.0:
            out[k] = amplitude[k]
            continue
        lo = np.searchsorted(freqs, f * 2.0 ** (-half_width_oct), "left")
        hi = np.searchsorted(freqs, f * 2.0 ** half_width_oct, "right")
        out[k] = np.median(amplitude[lo:hi])
    return out


def soft_reduction(amplitude: np.ndarray, freqs: np.ndarray, depth: float,
                   detail: np.ndarray, weight_db: np.ndarray | float = 0.0,
                   knee_center_db: float = 18.78301217932193,
                   knee_width_db: float = 9.010408026990198) -> np.ndarray:
    """Level-independent relative detector; depth zero is exactly unity."""
    amplitude = np.asarray(amplitude, dtype=np.float64)
    if depth == 0.0:
        return np.zeros_like(amplitude)
    background = octave_median(amplitude, freqs)
    exceed = 20.0 * np.log10((amplitude + EPS) / (background + EPS)) + weight_db
    ceiling = (16.167635300821875 * (depth / 5.0)
               * detail_strength(detail))
    reduction = ceiling * 0.5 * (
        1.0 + np.tanh((exceed - knee_center_db) / knee_width_db))
    return np.clip(reduction, 0.0, MAX_REDUCTION_DB)


def softplus_knee(z: np.ndarray, width_db: float = 6.0) -> np.ndarray:
    """Compressor-style C1 positive part used by absolute-level Hard mode."""
    z = np.asarray(z, dtype=np.float64)
    out = np.empty_like(z)
    below = z <= -0.5 * width_db
    above = z >= 0.5 * width_db
    middle = ~(below | above)
    out[below] = 0.0
    out[above] = z[above]
    out[middle] = (z[middle] + 0.5 * width_db) ** 2 / (2.0 * width_db)
    return out


def hard_reduction(amplitude: np.ndarray, depth: float,
                   weight_db: np.ndarray | float = 0.0,
                   slope: float = 0.75) -> np.ndarray:
    """Peak-amplitude calibrated absolute detector in dBFS."""
    amplitude = np.asarray(amplitude, dtype=np.float64)
    if depth == 0.0:
        return np.zeros_like(amplitude)
    level = 20.0 * np.log10(np.maximum(amplitude, EPS))
    threshold = -6.0 - 6.0 * depth
    return np.clip(slope * softplus_knee(level + weight_db - threshold),
                   0.0, MAX_REDUCTION_DB)


def link_amplitudes(amplitudes: np.ndarray, link: float) -> np.ndarray:
    """Phase-insensitive shared/dual interpolation in the power domain."""
    a = np.asarray(amplitudes, dtype=np.float64)
    if a.ndim != 2:
        raise ValueError("amplitudes must have shape (channels, bins)")
    linked = np.sqrt(np.mean(a * a, axis=0))
    return np.sqrt((1.0 - link) * a * a + link * linked[None, :] ** 2)


def focus_factors(focus: float) -> tuple[float, float]:
    return 1.0 - max(focus, 0.0), 1.0 - max(-focus, 0.0)


def ms_encode(stereo: np.ndarray) -> np.ndarray:
    """Energy-preserving L/R to M/S transform."""
    stereo = np.asarray(stereo, dtype=np.float64)
    return np.vstack(((stereo[0] + stereo[1]) / np.sqrt(2.0),
                      (stereo[0] - stereo[1]) / np.sqrt(2.0)))


def ms_decode(mid_side: np.ndarray) -> np.ndarray:
    """Inverse of :func:`ms_encode` (the orthonormal matrix is involutory)."""
    mid_side = np.asarray(mid_side, dtype=np.float64)
    return np.vstack(((mid_side[0] + mid_side[1]) / np.sqrt(2.0),
                      (mid_side[0] - mid_side[1]) / np.sqrt(2.0)))


def sum_focused_curves(curves_db: np.ndarray, focuses: np.ndarray,
                       channel: int) -> np.ndarray:
    """Sum enabled-band dB curves after per-band channel focus."""
    curves = np.asarray(curves_db, dtype=np.float64)
    factors = np.array([focus_factors(p)[channel] for p in focuses])
    return np.sum(curves * factors[:, None], axis=0)


def bandpass_union(band_masks: np.ndarray, focuses: np.ndarray | None = None,
                   channel: int = 0) -> np.ndarray:
    """Parallel bandpass routing union, including its continuous focus rule."""
    masks = np.asarray(band_masks, dtype=np.float64)
    if masks.size == 0:
        bins = masks.shape[-1] if masks.ndim == 2 else 1
        return np.ones(bins)
    masks = np.clip(masks, 0.0, 1.0)
    if focuses is None:
        return 1.0 - np.prod(1.0 - masks, axis=0)
    q = np.array([focus_factors(p)[channel] for p in focuses])
    union = 1.0 - np.prod(1.0 - q[:, None] * masks, axis=0)
    return np.clip(1.0 - np.max(q) + union, 0.0, 1.0)


def range_mask(freqs: np.ndarray, low_hz: float | None,
               high_hz: float | None, skirt_oct: float = 0.125) -> np.ndarray:
    """Cosine-skirted inclusive range mask; inactive boundaries mean unity."""
    f = np.asarray(freqs, dtype=np.float64)
    mask = np.ones_like(f)
    if low_hz is not None:
        lower = low_hz * 2.0 ** (-skirt_oct)
        x = np.clip(np.log2(np.maximum(f, lower) / lower) / skirt_oct, 0.0, 1.0)
        mask *= 0.5 - 0.5 * np.cos(np.pi * x)
        mask[f <= 0.0] = 0.0
    if high_hz is not None:
        upper = high_hz * 2.0 ** skirt_oct
        x = np.clip(np.log2(upper / np.minimum(np.maximum(f, EPS), upper))
                    / skirt_oct, 0.0, 1.0)
        mask *= 0.5 - 0.5 * np.cos(np.pi * x)
    return mask


def calibrated_rfft_amplitude(frame: np.ndarray, window: np.ndarray) -> np.ndarray:
    """Peak-amplitude-referred one-sided FFT calibration from the spec."""
    spectrum = np.fft.rfft(np.asarray(frame) * window)
    amplitude = 2.0 * np.abs(spectrum) / np.sum(window)
    amplitude[0] *= 0.5
    if len(frame) % 2 == 0:
        amplitude[-1] *= 0.5
    return amplitude


def _target_on_design_grid(freq_hz: np.ndarray, reduction_db: np.ndarray,
                           fs: float, design_fft: int) -> np.ndarray:
    grid = np.fft.rfftfreq(design_fft, 1.0 / fs)
    return np.clip(np.interp(grid, freq_hz, reduction_db),
                   0.0, MAX_REDUCTION_DB)


def minimum_phase_fir(freq_hz: np.ndarray, reduction_db: np.ndarray,
                      fs: float = FS, design_fft: int = DESIGN_FFT,
                      taps: int = MINIMUM_TAPS,
                      return_full: bool = False):
    """Real-cepstral minimum-phase design using natural logarithms.

    The unity curve is represented exactly as ``[1, 0, ...]``.  Truncation is
    reported and tested; no normalization or hidden terminal window is applied.
    """
    if design_fft % 2 or design_fft & (design_fft - 1):
        raise ValueError("design_fft must be an even power of two")
    reduction = _target_on_design_grid(freq_hz, reduction_db, fs, design_fft)
    if np.all(reduction == 0.0):
        h = np.zeros(taps)
        h[0] = 1.0
        if return_full:
            full = np.zeros(design_fft)
            full[0] = 1.0
            return h, full, reduction
        return h
    amplitude = np.power(10.0, -reduction / 20.0)
    log_half = np.log(amplitude)
    log_full = np.concatenate((log_half, log_half[-2:0:-1]))
    cepstrum = np.fft.ifft(log_full).real
    causal = np.zeros(design_fft)
    causal[0] = cepstrum[0]
    causal[1:design_fft // 2] = 2.0 * cepstrum[1:design_fft // 2]
    causal[design_fft // 2] = cepstrum[design_fft // 2]
    h_full = np.fft.ifft(np.exp(np.fft.fft(causal))).real
    h = h_full[:taps].copy()
    if return_full:
        return h, h_full, reduction
    return h


def linear_phase_fir(freq_hz: np.ndarray, reduction_db: np.ndarray,
                     fs: float = FS, design_fft: int = DESIGN_FFT,
                     delay: int = LINEAR_DELAY) -> np.ndarray:
    """Odd symmetric frequency-sampling FIR with exact integer delay."""
    reduction = _target_on_design_grid(freq_hz, reduction_db, fs, design_fft)
    amplitude = np.power(10.0, -reduction / 20.0)
    zero_phase = np.fft.irfft(amplitude, n=design_fft)
    offsets = np.arange(-delay, delay + 1)
    return zero_phase[offsets % design_fft]


@dataclass(frozen=True)
class FirKernel:
    taps: np.ndarray
    partition: int = PARTITION

    def __post_init__(self):
        h = np.asarray(self.taps, dtype=np.float64)
        padded = int(np.ceil(len(h) / self.partition) * self.partition)
        hp = np.pad(h, (0, padded - len(h)))
        parts = hp.reshape(-1, self.partition)
        spectra = np.fft.rfft(np.pad(parts, ((0, 0), (0, self.partition))), axis=1)
        object.__setattr__(self, "taps", hp)
        object.__setattr__(self, "spectra", spectra)
        object.__setattr__(self, "partitions", len(parts))


class CausalPartitionedFir:
    """Streaming direct-head/partitioned-tail convolver with no output FIFO.

    ``J+1`` spectra and ``L`` direct-history samples are fixed-capacity rings.
    Host calls can have arbitrary sizes; the internal partition counter alone
    controls FFT work.
    """

    def __init__(self, taps: np.ndarray, partition: int = PARTITION):
        self.kernel = FirKernel(taps, partition)
        self.P = partition
        self.J = self.kernel.partitions
        self.x_spectra = np.zeros((self.J + 1, self.P + 1), dtype=np.complex128)
        self.block_index = 0
        self.in_block = np.zeros(self.P)
        self.offset = 0
        self.history = np.zeros(len(self.kernel.taps))
        self.history_pos = 0
        self.tail = np.zeros(self.P)
        self._prepare_tail()

    @property
    def capacity(self) -> dict[str, int]:
        return {"P": self.P, "L": len(self.kernel.taps), "J": self.J,
                "input_spectra": self.J + 1, "fft_size": 2 * self.P}

    def _spectrum(self, block: int) -> np.ndarray:
        if block < 0 or self.block_index - block > self.J:
            return np.zeros(self.P + 1, dtype=np.complex128)
        return self.x_spectra[block % (self.J + 1)]

    def _overlap_block(self, block: int) -> np.ndarray:
        z = np.zeros(self.P + 1, dtype=np.complex128)
        for j in range(1, self.J):
            z += self.kernel.spectra[j] * self._spectrum(block - j)
        return np.fft.irfft(z, n=2 * self.P)

    def _prepare_tail(self) -> None:
        current = self._overlap_block(self.block_index)
        previous = self._overlap_block(self.block_index - 1)
        self.tail[:] = current[:self.P] + previous[self.P:]

    def replace_kernel_at_boundary(self, taps: np.ndarray) -> None:
        """Activate coefficients against retained history at a P boundary.

        Recomputing ``_prepare_tail`` evaluates both Z_b and Z_(b-1) with the
        new coefficients. This is the Python gate for the future preallocated
        bank activation operation; it is intentionally invalid mid-partition.
        """
        if self.offset != 0:
            raise ValueError("kernel activation requires an internal P boundary")
        kernel = FirKernel(taps, self.P)
        if kernel.partitions != self.J:
            raise ValueError("replacement must preserve allocated capacity")
        self.kernel = kernel
        self._prepare_tail()

    def process(self, samples: np.ndarray) -> np.ndarray:
        samples = np.asarray(samples, dtype=np.float64)
        out = np.empty_like(samples)
        head = self.kernel.taps[:self.P]
        for i, sample in enumerate(samples):
            self.history[self.history_pos] = sample
            indices = (self.history_pos - np.arange(self.P)) % len(self.history)
            out[i] = np.dot(head, self.history[indices]) + self.tail[self.offset]
            self.in_block[self.offset] = sample
            self.history_pos = (self.history_pos + 1) % len(self.history)
            self.offset += 1
            if self.offset == self.P:
                padded = np.pad(self.in_block, (0, self.P))
                self.x_spectra[self.block_index % (self.J + 1)] = np.fft.rfft(padded)
                self.block_index += 1
                self.offset = 0
                self.in_block.fill(0.0)
                self._prepare_tail()
        return out


def direct_fir(samples: np.ndarray, taps: np.ndarray) -> np.ndarray:
    """Independent literal convolution sum used as the renderer oracle."""
    x = np.asarray(samples, dtype=np.float64)
    h = np.asarray(taps, dtype=np.float64)
    y = np.zeros_like(x)
    for n in range(len(x)):
        stop = min(n + 1, len(h))
        y[n] = np.dot(h[:stop], x[n::-1][:stop])
    return y


def transition_direct_sum(samples: np.ndarray, old_taps: np.ndarray,
                          new_taps: np.ndarray, start: int,
                          fade_samples: int) -> np.ndarray:
    """Literal time-varying-kernel oracle for the specified raised cosine."""
    x = np.asarray(samples, dtype=np.float64)
    y = np.empty_like(x)
    for n in range(len(x)):
        if n < start:
            t = 0.0
        elif n >= start + fade_samples:
            t = 1.0
        else:
            q = n - start
            t = 0.5 - 0.5 * np.cos(np.pi * q / fade_samples)
        h = (1.0 - t) * old_taps + t * new_taps
        stop = min(n + 1, len(h))
        y[n] = np.dot(h[:stop], x[n::-1][:stop])
    return y


def transition_response(old_taps: np.ndarray, new_taps: np.ndarray,
                        fs: float, fade_samples: int,
                        wor_n: int = 65_537) -> tuple[float, float]:
    """Return max attenuation cap overshoot and above-unity ripple in dB."""
    _, old_h = signal.freqz(old_taps, worN=wor_n, fs=fs)
    _, new_h = signal.freqz(new_taps, worN=wor_n, fs=fs)
    max_cap = 0.0
    max_ripple = 0.0
    for q in range(fade_samples + 1):
        t = 0.5 - 0.5 * np.cos(np.pi * q / fade_samples)
        magnitude = np.abs((1.0 - t) * old_h + t * new_h)
        reduction = -20.0 * np.log10(np.maximum(magnitude, 1e-15))
        max_cap = max(max_cap, float(np.max(reduction - MAX_REDUCTION_DB)))
        max_ripple = max(max_ripple, float(np.max(-reduction)))
    return max_cap, max_ripple


@dataclass(frozen=True)
class TransitionPlan:
    """A cap-checked sequence of minimum-phase IRs admitted every P samples."""
    kernels: tuple[np.ndarray, ...]
    segments: int
    duration_samples: int
    cap_overshoot_db: float
    attenuation_ripple_db: float


def intermediate_transition_plan(freq_hz: np.ndarray,
                                 old_reduction_db: np.ndarray,
                                 new_reduction_db: np.ndarray, segments: int,
                                 fs: float = FS,
                                 design_fft: int = DESIGN_FFT,
                                 taps: int = MINIMUM_TAPS,
                                 partition: int = PARTITION) -> TransitionPlan:
    """Evaluate one explicit intermediate-curve experiment.

    This is deliberately not an adaptive scheduler or an approved production
    policy. It exists only to pin the rejected five-segment feasibility result
    while Stage 1 redesigns a cadence-preserving transition.
    """
    if segments < 1:
        raise ValueError("segments must be positive")
    old_r = np.clip(np.asarray(old_reduction_db), 0.0, MAX_REDUCTION_DB)
    new_r = np.clip(np.asarray(new_reduction_db), 0.0, MAX_REDUCTION_DB)
    kernels = tuple(minimum_phase_fir(
        freq_hz, (1.0 - k / segments) * old_r + (k / segments) * new_r,
        fs, design_fft, taps) for k in range(segments + 1))
    cap, ripple = 0.0, 0.0
    for a, b in zip(kernels[:-1], kernels[1:]):
        segment_cap, segment_ripple = transition_response(a, b, fs, partition)
        cap = max(cap, segment_cap)
        ripple = max(ripple, segment_ripple)
    return TransitionPlan(kernels, segments, segments * partition, cap, ripple)


def render_transition_plan(samples: np.ndarray, plan: TransitionPlan,
                           start: int = 0,
                           partition: int = PARTITION) -> tuple[np.ndarray, np.ndarray]:
    """Render a plan and its independent literal time-varying direct sum."""
    x = np.asarray(samples, dtype=np.float64)
    bank_outputs = [CausalPartitionedFir(h, partition).process(x)
                    for h in plan.kernels]
    rendered = np.empty_like(x)
    oracle = np.empty_like(x)
    for n in range(len(x)):
        relative = n - start
        if relative < 0:
            segment, q = 0, 0
        elif relative >= plan.duration_samples:
            segment, q = plan.segments - 1, partition
        else:
            segment, q = divmod(relative, partition)
        t = 0.5 - 0.5 * np.cos(np.pi * q / partition)
        rendered[n] = ((1.0 - t) * bank_outputs[segment][n]
                       + t * bank_outputs[segment + 1][n])
        h = ((1.0 - t) * plan.kernels[segment]
             + t * plan.kernels[segment + 1])
        stop = min(n + 1, len(h))
        oracle[n] = np.dot(h[:stop], x[n::-1][:stop])
    return rendered, oracle


def test_renderer() -> dict[str, float]:
    rng = np.random.default_rng(7103)
    P, L = PARTITION, 8 * PARTITION
    h = rng.normal(0.0, 0.035, L)
    # Exercise every specified boundary tap explicitly.
    h[:] *= np.exp(-np.arange(L) / 100.0)
    for index, value in [(P - 1, 0.17), (P, -0.21), (2 * P - 1, 0.13),
                         (2 * P, -0.11), (L - 1, 0.09)]:
        h[index] = value
    x = rng.normal(0.0, 0.2, 1_533)
    flush = np.zeros(L - 1)
    xf = np.concatenate((x, flush))
    scipy_y = signal.fftconvolve(x, h)[:len(xf)]
    direct_y = direct_fir(xf, h)
    conv = CausalPartitionedFir(h, P)
    chunks = [1, 7, 63, 2, 128, 5, 257, 3, 511, 64, 997]
    pieces, at = [], 0
    for size in chunks:
        if at >= len(xf):
            break
        pieces.append(conv.process(xf[at:at + size]))
        at += size
    if at < len(xf):
        pieces.append(conv.process(xf[at:]))
    got = np.concatenate(pieces)
    err_scipy = float(np.max(np.abs(got - scipy_y)))
    err_direct = float(np.max(np.abs(got - direct_y)))
    assert err_scipy < 1e-10 and err_direct < 1e-10
    impulse_error = 0.0
    for offset in range(P):
        probe = np.zeros(offset + L + 5)
        probe[offset] = 1.0
        c = CausalPartitionedFir(h, P)
        actual = c.process(probe)
        expected = signal.lfilter(h, [1.0], probe)
        impulse_error = max(impulse_error, float(np.max(np.abs(actual - expected))))
    assert impulse_error < 1e-10
    return {"scipy": err_scipy, "direct": err_direct,
            "host_partition": err_scipy, "impulse_offsets": impulse_error}


def _target_suite(fs: float, design_fft: int):
    f = np.fft.rfftfreq(design_fft, 1.0 / fs)
    gaussian = lambda f0, width, depth: depth * np.exp(
        -0.5 * ((f - f0) / (width / 2.355)) ** 2)
    return {
        "narrow54": gaussian(3000.0, 54.0, 24.0),
        "narrow54_cap": gaussian(3000.0, 54.0, 40.0),
        "broad500": gaussian(3000.0, 500.0, 18.0),
        "harmonic": sum(gaussian(k * 440.0, 90.0 + 3.0 * k, 12.0 / np.sqrt(k))
                        for k in range(1, 18)),
        "low_shelf": 30.0 / (1.0 + (f / 700.0) ** 4),
        "high_shelf": 30.0 / (1.0 + (7000.0 / np.maximum(f, 1e-9)) ** 4),
    }


def test_synthesis() -> dict[str, float]:
    fs, M, L = FS, DESIGN_FFT, MINIMUM_TAPS
    f = np.fft.rfftfreq(M, 1.0 / fs)
    max_full_relative = max_dense_db = max_ripple = max_tail = max_cap = 0.0
    for reduction in _target_suite(fs, M).values():
        h, full, target = minimum_phase_fir(f, reduction, fs, M, L, True)
        full_mag = np.abs(np.fft.rfft(full))
        wanted = 10.0 ** (-target / 20.0)
        max_full_relative = max(max_full_relative,
                                float(np.max(np.abs(full_mag - wanted) / wanted)))
        tail_fraction = float(np.sum(full[L:] ** 2) / np.sum(full ** 2))
        max_tail = max(max_tail, tail_fraction)
        dense_f, dense_h = signal.freqz(h, worN=131_073, fs=fs)
        dense_target = np.interp(dense_f, f, target)
        realized = -20.0 * np.log10(np.maximum(np.abs(dense_h), 1e-15))
        max_dense_db = max(max_dense_db, float(np.max(np.abs(realized - dense_target))))
        max_ripple = max(max_ripple, float(np.max(-realized)))
        max_cap = max(max_cap, float(np.max(realized) - np.max(target)))
    assert max_full_relative < 1e-10
    assert max_dense_db <= 0.25
    assert max_ripple <= 0.01
    assert max_cap <= 0.05
    unity = minimum_phase_fir(f, np.zeros_like(f), fs, M, L)
    assert np.array_equal(unity, np.r_[1.0, np.zeros(L - 1)])

    # Periodic Hilbert relation, independently obtained with scipy.signal.hilbert.
    target = _target_suite(fs, M)["harmonic"]
    _, full, reduction = minimum_phase_fir(f, target, fs, M, L, True)
    H = np.fft.fft(full)
    log_mag = np.log(np.maximum(np.abs(H), 1e-300))
    hilbert_phase = -np.imag(signal.hilbert(log_mag))
    phase = np.unwrap(np.angle(H))
    hilbert_error = float(np.max(np.abs((phase - phase[0]) -
                                        (hilbert_phase - hilbert_phase[0]))))
    assert hilbert_error < 1e-10

    # Small polynomial: all zeros strictly inside unit circle.  For
    # H(z)=sum(h[n]z^-n), multiplying by z^(L-1) leaves ``h`` (not h[::-1])
    # in descending-power order.  tf2zpk is an independent convention check.
    sf = np.linspace(0.0, fs / 2.0, M // 2 + 1)
    small = minimum_phase_fir(sf, 12.0 * np.exp(-0.5 * ((sf - 4000) / 1800) ** 2),
                              fs, M, 32)
    roots_numpy = np.roots(small)
    roots_scipy, _, _ = signal.tf2zpk(small, [1.0])
    root_reference_error = float(np.max(np.abs(np.sort_complex(roots_numpy) -
                                                np.sort_complex(roots_scipy))))
    max_root = float(np.max(np.abs(roots_numpy)))
    assert root_reference_error < 1e-12
    assert max_root < 1.0

    # The pinned scipy 1.13 API predates half=False.  Its homomorphic default
    # returns the spectral factor (half length), so compare that independent
    # reference on an autocorrelation whose factor is attenuation-only.  A
    # fixed n_fft makes scipy's approximation deterministic.
    known_factor = np.array([0.8, 0.1])
    symmetric = np.convolve(known_factor, known_factor[::-1])
    _, symmetric_h = signal.freqz(symmetric, worN=f, fs=fs)
    factor_reduction = -20.0 * np.log10(np.sqrt(np.abs(symmetric_h)))
    factor_ours = minimum_phase_fir(f, factor_reduction, fs, M, 2)
    factor_scipy = signal.minimum_phase(symmetric, method="homomorphic", n_fft=M)
    scipy_minimum_error = float(np.max(np.abs(factor_ours - factor_scipy)))
    assert scipy_minimum_error < 1e-6

    # Linear frequency-sampling construction against scipy firwin2, window=None.
    reduction = _target_suite(fs, M)["broad500"]
    ours = linear_phase_fir(f, reduction, fs, M, LINEAR_DELAY)
    reference = signal.firwin2(LINEAR_TAPS, f / (fs / 2.0),
                               10.0 ** (-reduction / 20.0), nfreqs=M // 2 + 1,
                               window=None)
    linear_error = float(np.max(np.abs(ours - reference)))
    symmetry_error = float(np.max(np.abs(ours - ours[::-1])))
    assert linear_error < 1e-12 and symmetry_error < 1e-12
    return {"full_grid_relative": max_full_relative,
            "dense_magnitude_db": max_dense_db, "attenuation_ripple_db": max_ripple,
            "endpoint_cap_overshoot_db": max_cap,
            "tail_energy_fraction": max_tail, "hilbert_rad": hilbert_error,
            "small_max_root": max_root, "root_reference": root_reference_error,
            "minimum_scipy": scipy_minimum_error, "linear_scipy": linear_error,
            "linear_symmetry": symmetry_error}


def test_transition_oracle() -> dict[str, float]:
    rng = np.random.default_rng(901)
    L, P, F = 8 * PARTITION, PARTITION, 256
    old = rng.normal(0, 0.02, L) * np.exp(-np.arange(L) / 90.0)
    new = rng.normal(0, 0.02, L) * np.exp(-np.arange(L) / 75.0)
    x = rng.normal(0, 0.2, 1_200)
    start = 2 * P
    a = CausalPartitionedFir(old, P)
    b = CausalPartitionedFir(new, P)
    old_y = a.process(x)
    new_y = b.process(x)
    q = np.arange(len(x)) - start
    t = np.where(q < 0, 0.0, np.where(q >= F, 1.0,
                 0.5 - 0.5 * np.cos(np.pi * q / F)))
    got = (1.0 - t) * old_y + t * new_y
    reference = transition_direct_sum(x, old, new, start, F)
    error = float(np.max(np.abs(got - reference)))
    assert error < 1e-10
    return {"direct_time_varying": error}


def cap_counterexample() -> dict[str, float]:
    """Reproduce the rejected endpoint-only crossfade with fixed numbers."""
    fs, M, L = FS, DESIGN_FFT, MINIMUM_TAPS
    f = np.fft.rfftfreq(M, 1.0 / fs)
    old_r = MAX_REDUCTION_DB / (1.0 + (1000.0 / np.maximum(f, 1e-9)) ** 4)
    new_r = MAX_REDUCTION_DB / (1.0 + (f / 10000.0) ** 4)
    old = minimum_phase_fir(f, old_r, fs, M, L)
    new = minimum_phase_fir(f, new_r, fs, M, L)
    dense_f, old_h = signal.freqz(old, worN=65_537, fs=fs)
    _, new_h = signal.freqz(new, worN=65_537, fs=fs)
    worst = (-np.inf, 0.0, 0.0)
    for q in range(257):
        t = 0.5 - 0.5 * np.cos(np.pi * q / 256.0)
        reduction = -20.0 * np.log10(np.maximum(
            np.abs((1.0 - t) * old_h + t * new_h), 1e-15))
        k = int(np.argmax(reduction - MAX_REDUCTION_DB))
        candidate = float(reduction[k] - MAX_REDUCTION_DB)
        if candidate > worst[0]:
            worst = candidate, float(dense_f[k]), t
    assert worst[0] > 1.0  # This gate documents a rejected algorithm.
    return {"endpoint_crossfade_cap_overshoot_db": worst[0],
            "frequency_hz": worst[1], "blend": worst[2]}


def test_controls() -> dict[str, float]:
    freqs = np.fft.rfftfreq(4096, 1.0 / FS)
    low, high = shelf_coordinates(freqs)
    endpoint_error = max(abs(low[np.argmin(abs(freqs - 500.0))] - 0.5),
                         abs(high[np.argmin(abs(freqs - 2000.0))] - 0.5))
    rng = np.random.default_rng(44)
    values = rng.uniform(0.0, 30.0, len(freqs))
    d = detail_profile(freqs, 5.0, 2.0, -2.0)
    direct = variable_box_smooth_direct(values, freqs, detail_width_oct(d))
    fast = variable_box_smooth(values, freqs, detail_width_oct(d))
    smoothing_error = float(np.max(np.abs(direct - fast)))
    assert smoothing_error < 1e-12

    # Detail monotonically narrows and strengthens at every bin.
    ds = [detail_profile(freqs, x) for x in np.linspace(0, 10, 101)]
    width_violation = float(np.max([np.max(detail_width_oct(ds[i + 1]) -
                                            detail_width_oct(ds[i]))
                                    for i in range(100)]))
    strength_violation = float(np.max([np.max(detail_strength(ds[i]) -
                                               detail_strength(ds[i + 1]))
                                       for i in range(100)]))
    assert width_violation <= 1e-12 and strength_violation <= 1e-12

    # Link endpoints and phase-insensitive anti-phase identity.
    a = rng.uniform(0.0, 1.0, (2, len(freqs)))
    link0 = float(np.max(np.abs(link_amplitudes(a, 0.0) - a)))
    linked = np.sqrt(np.mean(a * a, axis=0))
    link1 = float(np.max(np.abs(link_amplitudes(a, 1.0) - linked[None, :])))
    anti = np.vstack((a[0], -a[0]))
    anti_error = float(np.max(np.abs(link_amplitudes(np.abs(anti), 1.0)[0] - a[0])))
    assert max(link0, link1, anti_error) < 1e-12

    stereo = rng.normal(size=(2, 257))
    ms_roundtrip = float(np.max(np.abs(ms_decode(ms_encode(stereo)) - stereo)))
    energy_error = float(abs(np.sum(ms_encode(stereo) ** 2) - np.sum(stereo ** 2)))
    assert max(ms_roundtrip, energy_error) < 1e-12

    masks = rng.uniform(0.0, 1.0, (3, len(freqs)))
    union = bandpass_union(masks)
    union_reference = 1.0 - np.prod(1.0 - masks, axis=0)
    union_error = float(np.max(np.abs(union - union_reference)))
    disjoint = np.zeros((2, 8)); disjoint[0, :3] = 1; disjoint[1, 5:] = 1
    disjoint_error = float(np.max(np.abs(bandpass_union(disjoint) -
                                              np.array([1, 1, 1, 0, 0, 1, 1, 1]))))
    assert max(union_error, disjoint_error) < 1e-12

    focus_endpoint = max(abs(focus_factors(-1.0)[0] - 1.0),
                         abs(focus_factors(-1.0)[1]),
                         abs(focus_factors(1.0)[0]),
                         abs(focus_factors(1.0)[1] - 1.0))
    focused_a = bandpass_union(masks, np.array([-1.0, -1.0, -1.0]), 1)
    focused_b = bandpass_union(masks, np.array([1.0, 1.0, 1.0]), 0)
    focus_mask_unity = max(float(np.max(abs(focused_a - 1.0))),
                           float(np.max(abs(focused_b - 1.0))))
    assert max(focus_endpoint, focus_mask_unity) < 1e-12

    # Soft level invariance and Hard monotonic level/depth response.
    amp = rng.lognormal(-5.0, 1.0, len(freqs)); amp[256] *= 100
    soft_a = soft_reduction(amp, freqs, 5.0, d)
    soft_b = soft_reduction(amp * 10.0 ** (-36 / 20), freqs, 5.0, d)
    soft_spread = float(np.max(np.abs(soft_a - soft_b)))
    hard_levels = [hard_reduction(amp * 10.0 ** (db / 20), dep)
                   for dep in (1.0, 5.0, 10.0) for db in (-36, -18, 0)]
    hard_min_delta = min(float(np.min(hard_levels[i + 1] - hard_levels[i]))
                         for i in (0, 1, 3, 4, 6, 7))
    depth_min_delta = min(float(np.min(hard_levels[i + 3] - hard_levels[i]))
                          for i in range(6))
    # EPS intentionally breaks exact scale invariance infinitesimally near its
    # floor.  The spec asks this spread to be reported (its historical observed
    # bound is 0.31 dB), while exact 1e-12 applies to map identities/endpoints.
    assert soft_spread <= 0.31
    assert hard_min_delta >= -1e-12 and depth_min_delta >= -1e-12

    # Each of the six tilts independently changes its own low/high region.
    base_d = detail_profile(freqs, 5)
    detail_lo_delta = float(np.max(np.abs(detail_profile(freqs, 5, 1, 0) - base_d)))
    detail_hi_delta = float(np.max(np.abs(detail_profile(freqs, 5, 0, 1) - base_d)))
    base_a, base_r = time_constants(freqs, 5, 5)
    alo, _ = time_constants(freqs, 5, 5, attack_lo=1)
    ahi, _ = time_constants(freqs, 5, 5, attack_hi=1)
    _, rlo = time_constants(freqs, 5, 5, release_lo=1)
    _, rhi = time_constants(freqs, 5, 5, release_hi=1)
    six_min_effect = min(detail_lo_delta, detail_hi_delta,
                         float(np.max(abs(alo - base_a))), float(np.max(abs(ahi - base_a))),
                         float(np.max(abs(rlo - base_r))), float(np.max(abs(rhi - base_r))))
    assert six_min_effect > 0.0

    # Detector and decoupled envelope poles against scipy.signal.lfilter on
    # algebraically isolated one-pole branches.
    probe_tau = detector_tau(freqs, d)
    detector_state = np.zeros_like(freqs)
    detector_target = np.full_like(freqs, 12.0)
    detector_state = soft_detector_step(
        detector_target, detector_state, FS, 64, probe_tau)
    detector_closed = detector_target * (1.0 - np.exp(-64 / (FS * probe_tau)))
    detector_pole_error = float(np.max(abs(detector_state - detector_closed)))

    tau_a, tau_r = time_constants(freqs, 5.0, 5.0)
    bin_index = int(np.argmin(abs(freqs - 1000.0)))
    aa = np.exp(-64 / (FS * tau_a[bin_index]))
    ar = np.exp(-64 / (FS * tau_r[bin_index]))
    steps = 300
    # aR=0 isolates attack: release state equals the target exactly.
    y = np.zeros(1); state = np.zeros(1); attack_trace = []
    for _ in range(steps):
        y, state = decoupled_ballistics_step(
            np.array([10.0]), y, state, FS, 64,
            np.array([tau_a[bin_index]]), np.array([1e-30]))
        attack_trace.append(y[0])
    attack_ref = signal.lfilter([1.0 - aa], [1.0, -aa], np.full(steps, 10.0))
    attack_pole_error = float(np.max(abs(np.array(attack_trace) - attack_ref)))
    # aA=0 isolates release after an initialized 10 dB state.
    y = np.array([10.0]); state = np.array([10.0]); release_trace = []
    for _ in range(steps):
        y, state = decoupled_ballistics_step(
            np.zeros(1), y, state, FS, 64,
            np.array([1e-30]), np.array([tau_r[bin_index]]))
        release_trace.append(y[0])
    release_ref = 10.0 * ar ** np.arange(1, steps + 1)
    release_pole_error = float(np.max(abs(np.array(release_trace) - release_ref)))
    assert max(detector_pole_error, attack_pole_error, release_pole_error) < 1e-12

    # Generic window-amplitude calibration: periodic Hann on-bin sine, DC, and
    # Nyquist all return their known peak amplitudes with endpoint factor rules.
    N = 4096
    n = np.arange(N)
    window = signal.windows.hann(N, sym=False)
    sine_amp = calibrated_rfft_amplitude(
        0.37 * np.sin(2 * np.pi * 257 * n / N), window)[257]
    dc_amp = calibrated_rfft_amplitude(np.full(N, 0.23), window)[0]
    nyquist_amp = calibrated_rfft_amplitude(0.19 * (-1.0) ** n, window)[-1]
    amplitude_calibration_error = max(abs(sine_amp - 0.37), abs(dc_amp - 0.23),
                                      abs(nyquist_amp - 0.19))
    assert amplitude_calibration_error < 1e-12

    # A falling automated cap clamps old envelope state against the current
    # smoothed cap on every update; no stale deep reduction survives.
    envelope = np.full(len(freqs), 35.0)
    cap = 40.0
    cap_violation = 0.0
    for _ in range(100):
        envelope, cap = smooth_cap_and_clamp(envelope, cap, 6.0, FS, 64)
        cap_violation = max(cap_violation, float(np.max(envelope - cap)))
    assert cap_violation <= 1e-12 and cap > 6.0
    return {"shelf_endpoint_bin_error": endpoint_error,
            "smoothing_direct_prefix": smoothing_error,
            "detail_width_violation": width_violation,
            "detail_strength_violation": strength_violation,
            "link_identity": max(link0, link1, anti_error),
            "ms_roundtrip": ms_roundtrip, "ms_energy": energy_error,
            "mask_identity": max(union_error, disjoint_error, focus_mask_unity),
            "focus_endpoint": focus_endpoint,
            "soft_36db_spread": soft_spread,
            "hard_level_min_delta": hard_min_delta,
            "hard_depth_min_delta": depth_min_delta,
            "six_tilt_min_effect": six_min_effect,
            "soft_detector_pole": detector_pole_error,
            "attack_scipy_pole": attack_pole_error,
            "release_closed_pole": release_pole_error,
            "hann_amplitude_calibration": amplitude_calibration_error,
            "cap_history_violation": cap_violation,
            "smoothed_cap_after_100": cap}


def capacity_for_sample_rate(fs: float) -> tuple[int, int]:
    """Power-of-two capacity scaling that preserves the 48 kHz time span."""
    scale = 1
    while scale < fs / FS:
        scale *= 2
    return DESIGN_FFT * scale, MINIMUM_TAPS * scale


def test_sample_rates_and_modes() -> dict[str, float]:
    rate_dense_error = rate_ripple = 0.0
    max_m = max_l = 0
    for fs in (44_100.0, 48_000.0, 96_000.0, 192_000.0):
        M, L = capacity_for_sample_rate(fs)
        max_m, max_l = max(max_m, M), max(max_l, L)
        f = np.fft.rfftfreq(M, 1.0 / fs)
        reduction = 40.0 * np.exp(-0.5 * ((f - 3000.0) / (54.0 / 2.355)) ** 2)
        h = minimum_phase_fir(f, reduction, fs, M, L)
        dense_f, H = signal.freqz(h, worN=65_537, fs=fs)
        realized = -20.0 * np.log10(np.maximum(abs(H), 1e-15))
        wanted = np.interp(dense_f, f, reduction)
        rate_dense_error = max(rate_dense_error,
                               float(np.max(abs(realized - wanted))))
        rate_ripple = max(rate_ripple, float(np.max(-realized)))
    assert rate_dense_error <= 0.25 and rate_ripple <= 0.01

    # Identity/null and impulse latency: min phase reports B; linear B+D; low
    # latency forces B=0. The direct-head convolver adds no sample.
    rng = np.random.default_rng(19)
    x = rng.normal(0.0, 0.2, 7000)
    f = np.fft.rfftfreq(DESIGN_FFT, 1.0 / FS)
    h_min = minimum_phase_fir(f, np.zeros_like(f))
    h_lin = linear_phase_fir(f, np.zeros_like(f))
    latency_error = null_error = 0.0
    for kernel, explicit, reported in ((h_min, 2048, 2048),
                                       (h_lin, 2048, 2048 + LINEAR_DELAY),
                                       (h_min, 0, 0)):
        padded = np.concatenate((np.zeros(explicit), x))
        wet = signal.lfilter(kernel, [1.0], padded)
        aligned = np.concatenate((np.zeros(reported), x))[:len(wet)]
        null_error = max(null_error, float(np.max(abs(wet - aligned))))
        impulse = np.r_[1.0, np.zeros(reported + len(kernel) + 2)]
        impulse = signal.lfilter(kernel, [1.0],
                                 np.r_[np.zeros(explicit), impulse])
        peak = int(np.argmax(abs(impulse)))
        latency_error = max(latency_error, abs(peak - reported))
    assert null_error < 1e-10 and latency_error == 0
    return {"four_rate_dense_db": rate_dense_error,
            "four_rate_ripple_db": rate_ripple,
            "max_design_fft": float(max_m), "max_minimum_taps": float(max_l),
            "mode_null": null_error, "latency_samples": float(latency_error)}


def test_safe_transition_feasibility() -> dict[str, float]:
    f = np.fft.rfftfreq(DESIGN_FFT, 1.0 / FS)
    old_r = MAX_REDUCTION_DB / (1.0 + (1000.0 / np.maximum(f, 1e-9)) ** 4)
    new_r = MAX_REDUCTION_DB / (1.0 + (f / 10000.0) ** 4)
    plan = intermediate_transition_plan(f, old_r, new_r, segments=5)
    assert plan.cap_overshoot_db <= 0.05
    assert plan.attenuation_ripple_db <= 0.01
    rng = np.random.default_rng(411)
    x = rng.normal(0.0, 0.2, 900)
    rendered, oracle = render_transition_plan(x, plan, start=PARTITION)
    oracle_error = float(np.max(abs(rendered - oracle)))
    assert oracle_error < 1e-10
    # This passes numerical safety but is not a real-time policy approval: its
    # 320-sample duration exceeds High/Ultra's requested H.
    return {"segments": float(plan.segments),
            "duration_samples": float(plan.duration_samples),
            "duration_ms": 1000.0 * plan.duration_samples / FS,
            "cap_overshoot_db": plan.cap_overshoot_db,
            "attenuation_ripple_db": plan.attenuation_ripple_db,
            "direct_oracle": oracle_error}


def render_synthetic_evidence() -> list[Path]:
    """Render unclipped dry/minimum/linear synthetic evidence WAVs."""
    OUT.mkdir(parents=True, exist_ok=True)
    fs = int(FS)
    n = 2 * fs
    t = np.arange(n) / fs
    rng = np.random.default_rng(203)
    dry = (0.06 * rng.standard_normal(n)
           + 0.25 * np.sin(2 * np.pi * 3000 * t)
           + 0.08 * np.sin(2 * np.pi * 440 * t)
           + 0.07 * np.sin(2 * np.pi * (300 + 3500 * t) * t))
    f = np.fft.rfftfreq(DESIGN_FFT, 1.0 / FS)
    reduction = (24 * np.exp(-0.5 * ((f - 3000) / (80 / 2.355)) ** 2)
                 + 8 * np.exp(-0.5 * ((f - 440) / (120 / 2.355)) ** 2))
    h_min = minimum_phase_fir(f, reduction)
    h_lin = linear_phase_fir(f, reduction)
    wet_min = signal.lfilter(h_min, [1.0], dry)
    wet_lin = signal.lfilter(h_lin, [1.0], dry)
    paths = [OUT / "spectral_parity_synthetic_dry.wav",
             OUT / "spectral_parity_synthetic_minimum.wav",
             OUT / "spectral_parity_synthetic_linear_raw.wav"]
    for path, audio in zip(paths, (dry, wet_min, wet_lin)):
        wavfile.write(path, fs, audio.astype(np.float32))
    return paths


def run_all_gates(render: bool = True) -> dict[str, dict[str, float]]:
    tests = [("test 1 renderer indexing", test_renderer),
             ("test 2 FIR synthesis", test_synthesis),
             ("test 3 transition oracle", test_transition_oracle),
             ("test 4 rejected crossfade counterexample", cap_counterexample),
             ("test 5 controls", test_controls),
             ("test 6 sample rates and modes", test_sample_rates_and_modes),
             ("test 7 transition feasibility only", test_safe_transition_feasibility)]
    results = {}
    started = time.perf_counter()
    for label, function in tests:
        try:
            result = function()
            results[label] = result
            numbers = ", ".join(f"{k}={v:.12g}" for k, v in result.items())
            print(f"{label}: PASS ({numbers})")
        except Exception as exc:
            print(f"{label}: FAIL ({exc})")
            raise
    paths = render_synthetic_evidence() if render else []
    print("\nSUMMARY")
    print(f"capacity: M={DESIGN_FFT}, Lmin={MINIMUM_TAPS}, P={PARTITION}, "
          f"J={MINIMUM_TAPS // PARTITION}, spectra/history channel="
          f"{MINIMUM_TAPS // PARTITION + 1}, Llinear={LINEAR_TAPS}, D={LINEAR_DELAY}")
    print(f"elapsed_s={time.perf_counter() - started:.3f}")
    if paths:
        print("synthetic WAVs: " + ", ".join(str(p) for p in paths))
    print("VERDICT: PARTIAL PASS -- static renderer, synthesis, controls, and "
          "mode math verified. Full engine remains BLOCKED: endpoint-only "
          "transition rejected; safe five-segment experiment violates High/Ultra cadence.")
    return results


if __name__ == "__main__":
    run_all_gates()
