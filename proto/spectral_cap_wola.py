"""Bounded Max-cut repair: frame-gain limits, never a sample-domain promise.

Reuse the shipped legacy target computer and WOLA. Only retained y1 is capped;
s1 stays deep so release behavior is preserved. Off >=24 is legacy arithmetic.
"""
import json
import math
import numpy as np
import spectral_hard_wola as hw

LEGACY_BOUND = max(40.0, 16.167635300821875*2*4)
SNAP_DB = 1e-6


class Cap:
    def __init__(self, fs, requested):
        self.alpha = math.exp(-hw.HOP/(fs*.020))
        self.value = LEGACY_BOUND if requested >= 24 else requested
        self.bypass = requested >= 24

    def step(self, requested):
        target = LEGACY_BOUND if requested >= 24 else requested
        self.value = self.alpha*self.value+(1-self.alpha)*target
        if abs(self.value-target) <= SNAP_DB:
            self.value = target
        self.bypass = requested >= 24 and self.value == LEGACY_BOUND
        return self.value


def scalar_step(target, previous_y, previous_s, attack, release, cap, bypass):
    """Literal scalar oracle; no optimized control-law imports."""
    s = max(float(target), float(release)*(float(previous_s)-float(target))+float(target))
    y = float(attack)*(float(previous_y)-s)+s
    return (y if bypass else min(y, cap)), s


class CapWola(hw.HardWola):
    """Events are input sample indices: apply before accepting that sample.

    A hop ending exactly at an event still uses the previous request. Changes
    between hop boundaries use the latest request at the next completed hop.
    """
    def __init__(self, fs=48000., channels=1, p=None, mode=0, cap_events=None):
        super().__init__(fs, channels, p, mode)
        self.requested = self.p.max_cut_db
        self.cap = Cap(fs, self.requested)
        self.cap_events = dict(cap_events or {})
        self.caps, self.bypasses = [], []
        self.oracle_error = self.release_error = 0.
        for lane, core in enumerate(self.cores):
            original_gain, original_step = core.frame_gain_mode, core.ballistics.step

            def gain(mag, mode, lane=lane, original=original_gain):
                if lane == 0:
                    self.caps.append(self.cap.step(self.requested))
                    self.bypasses.append(self.cap.bypass)
                return original(mag, mode)

            def step(target, core=core, original=original_step):
                b = core.ballistics
                aa = np.broadcast_to(b.aA, target.shape)
                ar = np.broadcast_to(b.aR, target.shape)
                oracle = np.asarray([scalar_step(t, y, s, a, r, self.cap.value, self.cap.bypass)
                                     for t, y, s, a, r in zip(target, b.y1, b.s1, aa, ar)])
                y = original(target)
                if not self.cap.bypass:
                    y = np.minimum(y, self.cap.value)
                    b.y1 = y  # Store capped history, not merely the published result.
                self.oracle_error = max(self.oracle_error, float(np.max(np.abs(y-oracle[:, 0]))))
                self.release_error = max(self.release_error, float(np.max(np.abs(b.s1-oracle[:, 1]))))
                return y

            core.frame_gain_mode, core.ballistics.step = gain, step

    def process(self, samples):
        start, end = self.emitted, self.emitted+samples.shape[1]
        pieces, position = [], start
        for boundary in sorted(e for e in self.cap_events if start <= e < end):
            if boundary > position:
                pieces.append(super().process(samples[:, position-start:boundary-start]))
            self.requested = self.cap_events[boundary]
            for core in self.cores:
                core.max_cut_db = self.requested
            position = boundary
        pieces.append(super().process(samples[:, position-start:]))
        return np.concatenate(pieces, axis=1)


def render(samples, fs=48000., p=None, mode=0, cap_events=None, chunks=(137, 64, 1000, 1)):
    samples = np.atleast_2d(samples)
    length = samples.shape[1]
    padded = np.pad(samples, ((0, 0), (0, hw.N+(-length)%hw.HOP)))
    processor = CapWola(fs, samples.shape[0], p, mode, cap_events)
    parts, position, index = [], 0, 0
    while position < padded.shape[1]:
        count = chunks[index % len(chunks)]
        parts.append(processor.process(padded[:, position:position+count]))
        position, index = position+count, index+1
    return np.concatenate(parts, axis=1)[:, :length+hw.N], processor


def source(length=16384, fs=48000.):
    t = np.arange(length)/fs
    return np.asarray([.30*np.sin(2*np.pi*7500*t)+.03*np.sin(2*np.pi*731*t),
                       .17*np.sin(2*np.pi*9000*t)-.05*np.sin(2*np.pi*731*t)])


def run_all():
    # Deep synthetic history isolates the policy from detector and FFT errors.
    worst = 0.
    for fs in (44100., 48000., 96000., 192000.):
        cap = Cap(fs, 24.)
        c, y, s = LEGACY_BOUND, np.full(6, 110.), np.full(6, 120.)
        a, r = np.linspace(.1, .99, 6), np.linspace(.99, .99999, 6)
        stable_off = 0
        for request in [3.]*120+[17.]*100+[0.]*100+[24.]*3000+[6.]*100:
            target = LEGACY_BOUND if request >= 24 else request
            c = math.exp(-256/(fs*.020))*c+(1-math.exp(-256/(fs*.020)))*target
            if abs(c-target) <= SNAP_DB:
                c = target
            assert cap.step(request) == c
            demand = np.minimum(np.arange(6)*20., request) if request < 24 else np.arange(6)*20.
            oracle = np.asarray([scalar_step(t, yy, ss, aa, rr, c, cap.bypass)
                                 for t, yy, ss, aa, rr in zip(demand, y, s, a, r)])
            s = np.maximum(demand, r*(s-demand)+demand)
            y = a*(y-s)+s
            if not cap.bypass:
                y = np.minimum(y, c)
            worst = max(worst, float(np.max(np.abs(y-oracle[:, 0]))))
            assert np.array_equal(s, oracle[:, 1])
            if request == 0.:
                assert np.any(s > y)  # Clamping release memory would fail.
            stable_off += int(cap.bypass)
        assert stable_off > 0
        assert Cap(fs, 0.).step(0.) == 0.
        assert Cap(fs, 7.).step(7.) == 7.
    assert worst == 0.
    print(f"test 1 PASS scalar recurrence max_error_db={worst:g}; rates=4 bound_db={LEGACY_BOUND:.15g}")
    x, events = source(), {8193: 3., 11001: 17., 13003: 0., 14501: 24.}
    reconstruction = chunk_error = oracle_error = 0.
    deepest = 0.
    sideband = jump = 0.
    for ms in (False, True):
        for mode in (0, 1):
            p = hw.Params(depth=10., detail=10., attack_ms=1., release_ms=1000.,
                          tilt_detail=1., link_pct=0., ms_mode=ms)
            y, processor = render(x, p=p, mode=mode, cap_events=events)
            batch, _ = render(x, p=p, mode=mode, cap_events=events, chunks=(99999,))
            reference = hw.scipy_wola_reference(x, 48000., processor.gains, p)
            reconstruction = max(reconstruction, float(np.max(np.abs(y-reference))))
            chunk_error = max(chunk_error, float(np.max(np.abs(y-batch))))
            oracle_error = max(oracle_error, processor.oracle_error, processor.release_error)
            red, caps = np.asarray(processor.reductions), np.asarray(processor.caps)
            expected_cap = LEGACY_BOUND
            for frame, actual_cap in enumerate(caps):
                latest = [e for e in events if e < (frame+1)*hw.HOP]
                request = events[max(latest)] if latest else 24.
                target = LEGACY_BOUND if request >= 24 else request
                a = math.exp(-hw.HOP/(48000.*.020))
                expected_cap = a*expected_cap+(1-a)*target
                if abs(expected_cap-target) <= SNAP_DB:
                    expected_cap = target
                assert actual_cap == expected_cap
            assert np.min(red) >= -1e-12 and np.max(red-caps[:, None, None]) < 1e-10
            assert np.min(np.asarray(processor.gains)-10**(-caps[:, None, None]/20)) > -1e-12
            # Zero request decreases the effective limit on its declared ramp.
            zero_frame = (14501//256)-1
            assert caps[zero_frame] < caps[13003//256]
            assert np.max(red[zero_frame]) <= caps[zero_frame]+1e-10
            stable, stable_proc = render(x, p=p, mode=mode)
            legacy, _ = hw.render(x, p=p, mode=mode)
            assert np.array_equal(stable, legacy)
            deepest = max(deepest, float(np.max(stable_proc.reductions)))
            jump = max(jump, float(np.max(np.abs(np.diff(y[:, hw.N:], axis=1)))))
            delta = y[0, hw.N:]-stable[0, hw.N:]
            spectrum = np.fft.rfft(delta*np.hanning(len(delta)))
            keep = np.ones(len(spectrum), dtype=bool)
            for frequency in (731., 7500., 9000.):
                center = round(frequency*len(delta)/48000.)
                keep[max(0, center-4):center+5] = False
            sideband = max(sideband, float(np.linalg.norm(spectrum[keep])/len(delta)))
    assert deepest > 40. and reconstruction < 1e-10 and chunk_error == 0. and oracle_error == 0., (deepest, reconstruction, chunk_error, oracle_error)
    for initial in (0., 6.):
        p = hw.Params(max_cut_db=initial)
        for mode in (0, 1):
            _, fresh = render(x[:1, :2048], p=p, mode=mode)
            assert np.all(np.asarray(fresh.caps) == initial)
            assert np.max(fresh.reductions) <= initial+1e-10
    result = dict(scipy_max_error=reconstruction, chunk_error=chunk_error, scalar_error_db=oracle_error,
                  off_max_reduction_db=deepest, max_sample_difference=jump, off_carrier_residual_norm=sideband)
    print("test 2 PASS "+json.dumps(result, sort_keys=True))
    print("SUMMARY: capped history, shared hop clock, both modes/lanes/LR/MS, stable Off PASS")
    print("VERDICT: per-frame gain bound verified; no instantaneous audio bound or listening verdict")
    return result


if __name__ == "__main__":
    run_all()
