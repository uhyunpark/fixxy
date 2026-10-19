"""Additive Hard detector on EXACT legacy N1024/H256 sqrt-Hann WOLA.

Soft uses the untouched SidebarChannelCore. Hard has the verified absolute
knee, legacy frequency width, ms timing/time tilts, MAX link and weight routing.
Legacy tiltDetail strength is deliberately Soft-only (root decision).
This is our approximation; original soothe comparison is unavailable.
"""
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
import json
import warnings

import numpy as np
from scipy import signal
from scipy.io import wavfile
import spectral_core as sc
import spectral_sidebar as ss
from spectral_stft import sqrt_hann, cola_norm
from spectral_parity import hard_reduction
from dump_spectral_fixtures import frozen_params

N, HOP = 1024, 256


def calibrated_amplitude(magnitude):
    result = 2*np.asarray(magnitude)/np.sum(sqrt_hann(N))
    result = result.copy()
    result[..., [0, -1]] *= 0.5
    return result


@dataclass
class Params:
    depth: float = 5.0
    detail: float = 5.0
    attack_ms: float = 27.0
    release_ms: float = 50.0
    bands: list = field(default_factory=list)
    lo: float = 20.0
    hi: float = 20000.0
    max_cut_db: float = 24.0
    ms_mode: bool = False
    link_pct: float = 100.0
    focus: float = 0.0
    tilt_attack: float = 0.0
    tilt_release: float = 0.0
    tilt_detail: float = 0.0


class DetectionCore(ss.SidebarChannelCore):
    def __init__(self, fs, p, depth):
        super().__init__(N, fs, frozen_params(), depth, p.detail, p.attack_ms,
                         p.release_ms, HOP, p.bands, p.lo, p.hi, p.max_cut_db,
                         p.tilt_attack, p.tilt_release, p.tilt_detail)

    def frame_gain_mode(self, mag, mode):
        # No reimplementation/normalization of the legacy Soft branch.
        if mode == 0:
            return super().frame_gain(mag)
        if not self.active:
            return np.ones_like(mag)
        if not np.any(mag > 0.0):
            raw = np.zeros_like(mag)
            return 10**(-self.ballistics.step(raw)/20)
        # Focus is local depth, never detector input gain. tiltDetail is Soft-only.
        raw = hard_reduction(calibrated_amplitude(mag), self.depth, self.weight)
        if self.max_cut_db < 24.0:
            raw = np.minimum(raw, self.max_cut_db)
        return 10**(-self.ballistics.step(self.smoother(raw))/20)


class HardWola:
    """Exact node hop ordering. One existing s1/y1 pair survives mode changes.

    events maps completed-hop sample counts to the latest detectionMode (0/1).
    Mode changes only the target computer; no reset, bank, fade or second FFT.
    Prototype FIFO/output growth is NOT a production allocation contract.
    """
    def __init__(self, fs=48000.0, channels=1, p=None, mode=0, events=None):
        self.fs, self.channels, self.p = fs, channels, p or Params()
        self.mode, self.events = mode, events or {}
        self.cores = [DetectionCore(fs, self.p, self.p.depth*ss.focus_depth_bias(s, self.p.focus))
                      for s in (1, -1)]
        self.dual = channels == 2 and (self.p.link_pct < 100 or self.p.focus != 0)
        self.window = sqrt_hann(N)
        self.norm = cola_norm(N, HOP)[0]
        self.ring = np.zeros((channels, N))
        self.ola = np.zeros_like(self.ring)
        self.fifo = deque()
        self.completed = np.zeros((channels, 0))
        self.hops = self.emitted = 0
        self.gains, self.reductions, self.modes = [], [], []

    def process(self, samples):
        samples = np.asarray(samples, dtype=float)
        assert samples.ndim == 2 and samples.shape[0] == self.channels
        self.fifo.extend(samples.T.tolist())
        while len(self.fifo) >= HOP:
            chunk = np.asarray([self.fifo.popleft() for _ in range(HOP)]).T
            self.ring = np.concatenate((self.ring[:, HOP:], chunk), axis=1)
            frame = self.ring*self.window
            ms = self.p.ms_mode and self.channels == 2
            if ms:
                frame = np.asarray(ss.encode_ms(*frame))
            spectrum = np.fft.rfft(frame, axis=1)
            self.mode = self.events.get((self.hops+1)*HOP, self.mode)
            mag = np.abs(spectrum)
            if self.dual:
                detector = self.p.link_pct/100*np.max(mag, axis=0)+(1-self.p.link_pct/100)*mag
                gain = np.asarray([c.frame_gain_mode(m, self.mode)
                                   for c, m in zip(self.cores, detector)])
            else:
                g = self.cores[0].frame_gain_mode(np.max(mag, axis=0), self.mode)
                gain = np.tile(g, (self.channels, 1))
            self.gains.append(gain.copy())
            self.reductions.append(-20*np.log10(gain))
            self.modes.append(self.mode)
            frame = np.fft.irfft(spectrum*gain, n=N, axis=1)
            if ms:
                frame = np.asarray(ss.decode_ms(*frame))
            self.ola += frame*self.window
            self.hops += 1
            if self.hops*HOP-N >= 0:
                self.completed = np.concatenate((self.completed, self.ola[:, :HOP]*self.norm), axis=1)
            self.ola = np.concatenate((self.ola[:, HOP:], np.zeros((self.channels, HOP))), axis=1)
        output = np.zeros_like(samples)
        first = max(N-self.emitted, 0)
        if first < samples.shape[1]:
            start = self.emitted+first-N
            output[:, first:] = self.completed[:, start:start+samples.shape[1]-first]
        self.emitted += samples.shape[1]
        return output


def render(samples, fs=48000.0, p=None, mode=0, events=None, chunks=(137, 64, 1000, 1)):
    samples = np.atleast_2d(samples)
    length = samples.shape[1]
    padded = np.pad(samples, ((0, 0), (0, N+(-length)%HOP)))
    processor = HardWola(fs, samples.shape[0], p, mode, events)
    outputs, position, index = [], 0, 0
    while position < padded.shape[1]:
        count = chunks[index % len(chunks)]
        outputs.append(processor.process(padded[:, position:position+count]))
        position += count
        index += 1
    return np.concatenate(outputs, axis=1)[:, :length+N], processor


def scipy_wola_reference(samples, fs, gains, p):
    """Independent scipy STFT/ISTFT framing with recorded gain decisions.

    scipy scales STFT by sum(w); ISTFT normalizes squared-window overlap.
    Our central samples have constant overlap 2, matching node norm=0.5.
    """
    samples = np.atleast_2d(samples)
    padded = np.pad(samples, ((0, 0), (N-HOP, N+(-samples.shape[1])%HOP)))
    if p.ms_mode and samples.shape[0] == 2:
        padded = np.asarray(ss.encode_ms(*padded))
    _, _, z = signal.stft(padded, fs, window=sqrt_hann(N), nperseg=N,
                          noverlap=N-HOP, boundary=None, padded=False, axis=-1)
    assert z.shape[-1] == len(gains)
    modified = z*np.asarray(gains).transpose(1, 2, 0)
    with warnings.catch_warnings():
        # Only the padded first sample has zero Hann coverage; we discard it.
        warnings.filterwarnings("ignore", message="NOLA condition failed")
        _, reconstructed = signal.istft(modified, fs, window=sqrt_hann(N), nperseg=N,
                                        noverlap=N-HOP, boundary=False)
    if p.ms_mode and samples.shape[0] == 2:
        reconstructed = np.asarray(ss.decode_ms(*reconstructed))
    aligned = reconstructed[:, N-HOP:N-HOP+samples.shape[1]]
    return np.pad(aligned, ((0, 0), (N, 0)))


def test_calibration():
    """Missing actual-window normalization must fail on a known on-bin tone."""
    n = np.arange(N)
    x = 0.25*np.sin(2*np.pi*64*n/N)
    amplitude = calibrated_amplitude(np.abs(np.fft.rfft(x*sqrt_hann(N))))
    assert abs(amplitude[64]-0.25) < 1e-5, amplitude[64]
    worst = 0.0
    biases = {}
    for offset in (0, 0.125, 0.25, 0.5):
        x = 0.25*np.sin(2*np.pi*(64+offset)*n/N+0.31)
        bins = (63, 64, 65, 66)
        spectrum = np.fft.rfft(x*sqrt_hann(N))
        # Direct DFT sums independent of FFT/calibration helper.
        direct = np.asarray([np.dot(x*sqrt_hann(N), np.exp(-2j*np.pi*k*n/N)) for k in bins])
        expected = 2*np.abs(direct)/np.sum(sqrt_hann(N))
        worst = max(worst, float(np.max(np.abs(calibrated_amplitude(np.abs(spectrum))[list(bins)]-expected))))
        biases[str(offset)] = float(20*np.log10(np.max(calibrated_amplitude(np.abs(spectrum)))/0.25))
        # Independent time-domain fit recovers the source amplitude; no peak correction
        # is silently inserted into the detector to make off-bin results look exact.
        fit = np.linalg.lstsq(np.column_stack((np.sin(2*np.pi*(64+offset)*n/N),
                                             np.cos(2*np.pi*(64+offset)*n/N))), x, rcond=None)[0]
        assert abs(np.linalg.norm(fit)-0.25) < 1e-12
    for k, x in ((0, np.full(N, 0.25)), (N//2, 0.25*(-1.)**n)):
        assert abs(calibrated_amplitude(np.abs(np.fft.rfft(x*sqrt_hann(N))))[k]-0.25) < 1e-12
    assert worst < 1e-12
    return {"on_bin_absolute_error": float(abs(amplitude[64]-0.25)),
            "direct_dft_error": worst, "peak_bin_bias_db": biases}


def test_soft_unchanged_and_reference():
    """Wrong legacy Soft branch, framing, routing or calibration fails here."""
    rng = np.random.default_rng(31)
    t = np.arange(12001)/48000
    x = np.asarray([0.2*np.sin(2*np.pi*3000*t)+0.005*rng.standard_normal(len(t)),
                    0.1*np.sin(2*np.pi*3017*t)+0.005*rng.standard_normal(len(t))])
    worst_soft = worst_scipy = worst_chunk = 0.0
    cases = [Params(), Params(detail=10, ms_mode=True, link_pct=0),
             Params(detail=2, focus=0.4, link_pct=50, max_cut_db=6,
                    tilt_detail=-0.5, tilt_attack=0.5, tilt_release=-0.3,
                    bands=[{"on": True, "type": 0, "freq_hz": 3000., "gain_db": 12., "q": .7}], lo=200, hi=8000)]
    for p in cases:
        y, processor = render(x, p=p)
        # Existing spectral_sidebar.run_sidebar has a documented extra-N batch
        # slice. Construct its unchanged cores/gain hook but take the correctly
        # input-aligned raw WOLA output, matching current streaming SpectralNode.
        source = np.asarray(ss.encode_ms(*x)) if p.ms_mode else x
        bias = (ss.focus_depth_bias(1, p.focus), ss.focus_depth_bias(-1, p.focus))
        cores = [ss.SidebarChannelCore(N, 48000, frozen_params(), p.depth*bias[ch],
                  p.detail, p.attack_ms, p.release_ms, HOP, p.bands, p.lo, p.hi,
                  p.max_cut_db, p.tilt_attack, p.tilt_release, p.tilt_detail)
                 for ch in range(2)]
        legacy = np.asarray(ss._stereo_hop_loop(*source, HOP, N,
                            ss._sidebar_gain_fn(*cores, p.link_pct)))[:, :x.shape[1]]
        if p.ms_mode:
            legacy = np.asarray(ss.decode_ms(*legacy))
        worst_soft = max(worst_soft, float(np.max(np.abs(y[:, N:]-legacy))))
        for mode in (0, 1):
            y, processor = render(x, p=p, mode=mode)
            reference = scipy_wola_reference(x, 48000, processor.gains, p)
            worst_scipy = max(worst_scipy, float(np.max(np.abs(y-reference))))
            batch, _ = render(x, p=p, mode=mode, chunks=(50000,))
            worst_chunk = max(worst_chunk, float(np.max(np.abs(y-batch))))
    assert worst_soft < 1e-10, worst_soft
    assert worst_scipy < 1e-10, worst_scipy
    assert worst_chunk == 0
    return {"legacy_soft_max_error": worst_soft, "scipy_audio_max_error": worst_scipy,
            "chunk_max_error": worst_chunk}


def sine_amplitude(samples, frequency, fs):
    t = np.arange(len(samples))/fs
    fit = np.linalg.lstsq(np.column_stack((np.sin(2*np.pi*frequency*t),
                                         np.cos(2*np.pi*frequency*t))), samples, rcond=None)[0]
    return np.linalg.norm(fit)


def test_absolute_depth_and_material():
    """A relative detector or wrong depth direction fails source-level sweeps."""
    fs = 48000
    t = np.arange(fs)/fs
    transfer, worst_depth = {}, 0.0
    for offset in (0, 0.5):
        f = (64+offset)*fs/N
        attenuation = []
        for level in (-60, -36, -24, -12, -6):
            x = 10**(level/20)*np.sin(2*np.pi*f*t+0.3)
            y, processor = render(x, mode=1)
            attenuation.append(float(20*np.log10(sine_amplitude(x[fs//2:], f, fs)/
                                                sine_amplitude(y[0, N+fs//2:], f, fs))))
        assert np.min(np.diff(attenuation)) >= -1e-10
        transfer[str(offset)] = attenuation
        x = .25*np.sin(2*np.pi*f*t)
        previous = None
        for depth in (0, 1, 3, 5, 7, 10):
            y, processor = render(x, p=Params(depth=depth), mode=1)
            current = np.asarray(processor.reductions)[-30:-4]
            if previous is not None:
                worst_depth = max(worst_depth, float(np.max(previous-current)))
            previous = current
        assert worst_depth < 1e-10
    silence, _ = render(np.zeros(4096), mode=1)
    assert np.max(np.abs(silence)) == 0
    rng = np.random.default_rng(9)
    materials = {"broadband": .05*rng.standard_normal(fs),
                 "harmonics": sum(.2/k*np.sin(2*np.pi*220*k*t) for k in range(1, 17))}
    measured = {}
    for name, x in materials.items():
        measured[name] = {}
        for mode in (0, 1):
            y, processor = render(x, mode=mode)
            output = y[0, N+fs//2:]
            measured[name]["soft" if mode == 0 else "hard"] = float(20*np.log10(np.linalg.norm(x[fs//2:])/np.linalg.norm(output)))
            assert np.all(np.isfinite(output))
            assert np.max(np.asarray(processor.reductions)) < 40+1e-10
    return {"levels_dbfs": [-60, -36, -24, -12, -6], "audio_attenuation_db": transfer,
            "depth_monotonicity_excess_db": worst_depth, "silence_max": 0.0,
            "material_rms_attenuation_db": measured}


def test_switch():
    """Resetting state or following host chunks instead of hop events fails."""
    fs = 48000
    t = np.arange(48000)/fs
    x = .3*np.sin(2*np.pi*3000*t)+.1*np.sin(2*np.pi*7000*t)
    events = {12288: 1, 24576: 0, 36864: 1, 37120: 0}
    worst_reference = worst_chunk = worst_pole = 0.0
    measured = {}
    for attack in (1., 27., 250.):
        p = Params(attack_ms=attack)
        y, processor = render(x, p=p, events=events)
        reference = scipy_wola_reference(x, fs, processor.gains, p)
        batch, _ = render(x, p=p, events=events, chunks=(65536,))
        worst_reference = max(worst_reference, float(np.max(np.abs(y-reference))))
        worst_chunk = max(worst_chunk, float(np.max(np.abs(y-batch))))
        red = np.asarray(processor.reductions)[:, 0]
        # Independent analytic first-order bound: existing decoupled attack
        # cannot jump further than (1-aA)*max(current release, previous output).
        pole = np.exp(-HOP/(fs*attack/1000))
        bound = (1-pole)*40
        worst_pole = max(worst_pole, float(np.max(np.abs(np.diff(red, axis=0)))-bound))
        derivative = np.diff(y[0, N:])
        peaks = [float(np.max(np.abs(derivative[max(0,s-N):s+N]))) for s in events]
        measured[str(attack)] = {"max_sample_difference_near_events": max(peaks),
                                "max_bin_frame_change_db": float(np.max(np.abs(np.diff(red, axis=0)))),
                                "pole_bound_db": float(bound)}
    assert worst_reference < 1e-10
    assert worst_chunk == 0
    assert worst_pole < 1e-10
    # Pure zero input does not regenerate reduction in Hard after a loud history.
    processor = HardWola(fs, mode=1)
    processor.process(np.tile(x[:N], 4)[None, :])
    before = processor.cores[0].ballistics.y1.copy()
    processor.process(np.zeros((1, 2*fs)))
    after = processor.cores[0].ballistics.y1
    assert np.max(after) < np.max(before)*1e-8
    return {"scipy_switch_audio_max_error": worst_reference, "chunk_max_error": worst_chunk,
            "pole_bound_excess_db": worst_pole, "switch_measurements": measured,
            "release_after_two_seconds_db": float(np.max(after))}


def render_evidence():
    fs = 48000
    t = np.arange(2*fs)/fs
    rng = np.random.default_rng(44)
    x = .005*rng.standard_normal(len(t))
    x += .2*np.sin(2*np.pi*220*t)+.06*np.sin(2*np.pi*440*t)
    x += np.where((t>.4)&(t<1.4), .25*np.sin(2*np.pi*3017*t), 0)
    directory = Path("proto/out")
    directory.mkdir(exist_ok=True)
    paths = []
    for name, mode in (("dry", None), ("soft", 0), ("hard", 1)):
        y = x if mode is None else render(x, mode=mode)[0][0, N:]
        path = directory/f"spectral_hard_wola_{name}.wav"
        wavfile.write(path, fs, y.astype(np.float32))
        paths.append(str(path))
    return paths


def run_all(render_wavs=True):
    tests = [test_calibration, test_soft_unchanged_and_reference,
             test_absolute_depth_and_material, test_switch]
    results = {}
    for index, test in enumerate(tests, 1):
        result = test()
        results[test.__name__] = result
        print(f"test {index} PASS {json.dumps(result, sort_keys=True)}")
    if render_wavs:
        results["synthetic_wavs"] = render_evidence()
    print("SUMMARY: scoped additive Hard WOLA gates PASS")
    print("VERDICT: not complete parity; no original-soothe comparison or listening verdict")
    return results


if __name__ == "__main__":
    run_all()
