"""External detector and independent key monitor on the frozen legacy WOLA.

This is our design, not a claim about proprietary soothe internals. Key audio
controls gain but never replaces the main carrier. Python queues/arrays are
prototype conveniences; C++ must preallocate the additional key FIFO/rings.
"""
from collections import deque
import json
import math
import numpy as np
from scipy.ndimage import median_filter
from scipy.io import wavfile
from pathlib import Path

import spectral_hard_wola as hw

N, HOP = hw.N, hw.HOP


def detector_magnitudes(key_spectrum, key_channels, main_channels, ms_mode):
    """Mono key excites BOTH detector lanes, including an M/S main bus.

    Stereo-to-mono power averaging cannot cancel an antiphase key. Stereo key
    is encoded into M/S only when a stereo carrier is actually encoded M/S.
    """
    if key_channels == 0:
        return np.zeros((main_channels, N//2+1))
    if key_channels == 1:
        return np.tile(np.abs(key_spectrum[0]), (main_channels, 1))
    if main_channels == 1:
        return np.sqrt(np.mean(np.abs(key_spectrum)**2, axis=0))[None, :]
    if ms_mode:
        key_spectrum = np.asarray(hw.ss.encode_ms(*key_spectrum.real)) + 1j*np.asarray(
            hw.ss.encode_ms(*key_spectrum.imag))
    return np.abs(key_spectrum)


class SidechainWola(hw.HardWola):
    """Separate key history, same N/H framing and retained detector state.

    events: {completed_input_sample: (external_enabled, detection_mode)}.
    Connection count is sampled from the final sample of each completed hop.
    Connected history is collected even while external detection is disabled.
    """
    def __init__(self, fs=48000., channels=2, p=None, mode=0,
                 enabled=True, events=None):
        super().__init__(fs, channels, p, mode)
        self.enabled, self.source_events = enabled, events or {}
        self.key_fifo, self.count_fifo = deque(), deque()
        self.key_ring = np.zeros((2, N))
        self.detectors, self.release_states, self.connections = [], [], []

    def process(self, samples, key=None):
        samples = np.asarray(samples, dtype=float)
        assert samples.ndim == 2 and samples.shape[0] == self.channels
        count = 0 if key is None else np.asarray(key).shape[0]
        assert count in (0, 1, 2)
        physical = np.zeros((2, samples.shape[1]))
        if count:
            assert np.asarray(key).shape[1] == samples.shape[1]
            physical[:count] = key
        self.fifo.extend(samples.T.tolist())
        self.key_fifo.extend(physical.T.tolist())
        self.count_fifo.extend([count]*samples.shape[1])
        while len(self.fifo) >= HOP:
            chunk = np.asarray([self.fifo.popleft() for _ in range(HOP)]).T
            key_chunk = np.asarray([self.key_fifo.popleft() for _ in range(HOP)]).T
            counts = [self.count_fifo.popleft() for _ in range(HOP)]
            self.ring = np.concatenate((self.ring[:, HOP:], chunk), axis=1)
            self.key_ring = np.concatenate((self.key_ring[:, HOP:], key_chunk), axis=1)
            frame = self.ring*self.window
            ms = self.p.ms_mode and self.channels == 2
            if ms:
                frame = np.asarray(hw.ss.encode_ms(*frame))
            spectrum = np.fft.rfft(frame, axis=1)
            key_spectrum = np.fft.rfft(self.key_ring*self.window, axis=1)
            self.enabled, self.mode = self.source_events.get(
                (self.hops+1)*HOP, (self.enabled, self.mode))
            mag = (detector_magnitudes(key_spectrum, counts[-1], self.channels, ms)
                   if self.enabled else np.abs(spectrum))
            self.detectors.append(mag.copy())
            self.connections.append(counts[-1])
            detector = (self.p.link_pct/100*np.max(mag, axis=0)
                        +(1-self.p.link_pct/100)*mag) if self.dual else np.max(mag, axis=0)[None, :]
            gain, demand = [], []
            for core, lane in zip(self.cores, detector):
                # The legacy Soft tanh has nonzero zero-input baseline. External
                # exact silence MUST instead release toward zero demand.
                if self.enabled and not np.any(lane > 0.) and core.active:
                    g = 10**(-core.ballistics.step(np.zeros_like(lane))/20)
                else:
                    g = core.frame_gain_mode(lane, self.mode)
                gain.append(g)
                demand.append(core.ballistics.s1.copy())
            if not self.dual:
                gain = gain*self.channels
            gain = np.asarray(gain)
            self.release_states.append(np.asarray(demand))
            self.gains.append(gain.copy())
            self.reductions.append(-20*np.log10(gain))
            self.modes.append(self.mode)
            frame = np.fft.irfft(spectrum*gain, n=N, axis=1)
            if ms:
                frame = np.asarray(hw.ss.decode_ms(*frame))
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


def render(main, key, p=None, mode=0, enabled=True, events=None,
           connection_events=None, chunks=(137, 1, 480, 64), fs=48000.):
    """Connection events use zero-based INPUT sample positions; controls use hops.

    key is physical 0/1/2-channel audio, padded with zero for the render tail.
    A connection event may select any count <= supplied key channel count.
    """
    main = np.atleast_2d(main)
    key = np.zeros((0, main.shape[1])) if key is None else np.atleast_2d(key)
    length = main.shape[1]
    pad = N+(-length)%HOP
    main = np.pad(main, ((0, 0), (0, pad)))
    key = np.pad(key, ((0, 0), (0, pad)))
    changes = {0: key.shape[0], **(connection_events or {})}
    processor = SidechainWola(fs, main.shape[0], p, mode, enabled, events)
    outputs, position, block, count = [], 0, 0, key.shape[0]
    while position < main.shape[1]:
        count = changes.get(position, count)
        assert 0 <= count <= key.shape[0]
        next_change = min((v for v in changes if v > position), default=main.shape[1])
        end = min(position+chunks[block % len(chunks)], next_change, main.shape[1])
        outputs.append(processor.process(main[:, position:end], key[:count, position:end] if count else None))
        position, block = end, block+1
    return np.concatenate(outputs, axis=1)[:, :length+N], processor


def scalar_gain_oracle(mag, core, mode, external, s1, y1):
    """Independent binwise control loop, scipy median and direct boxcar sums.

    Reuse verified static coefficient tables only, never production detector,
    smoother or state-update functions. Returns gain plus next scalar states.
    """
    if not core.active:
        return np.ones_like(mag), s1, y1, np.zeros_like(mag)
    zero = external and not np.any(mag > 0.)
    if zero:
        raw = np.zeros_like(mag)
    elif mode == 0:
        logf = np.log2(core.freqs[1:])
        grid = np.linspace(logf[0], logf[-1], 512)
        # scipy median independently realizes the frozen log-grid neighborhood.
        env_grid = median_filter(np.interp(grid, logf, mag[1:]),
                                 size=core.env.k, mode="nearest")
        env = np.r_[mag[0], np.interp(logf, grid, env_grid)]
        raw = np.asarray([float(np.broadcast_to(core.max_db, mag.shape)[k])*.5*(1+math.tanh(
            (20*math.log10((mag[k]+1e-12)/(env[k]+1e-12))+core.weight[k]-core.p['knee_c'])
            /core.p['knee_w'])) for k in range(len(mag))])
    else:
        amplitude = 2*mag/sum(math.sqrt(.5-.5*math.cos(2*math.pi*n/N)) for n in range(N))
        amplitude[[0, -1]] *= .5
        raw = []
        for k, a in enumerate(amplitude):
            z = 20*math.log10(max(a, 1e-12))+core.weight[k]+6+6*core.depth
            knee = 0. if z <= -3 else z if z >= 3 else (z+3)**2/12
            raw.append(min(40., max(0., .75*knee)))
        raw = np.asarray(raw)
    if core.max_cut_db < 24:
        raw = np.minimum(raw, core.max_cut_db)
    target = np.asarray([np.mean(raw[lo:hi+1]) for lo, hi in zip(core.smoother.lo, core.smoother.hi)])
    aA = np.broadcast_to(core.ballistics.aA, mag.shape)
    aR = np.broadcast_to(core.ballistics.aR, mag.shape)
    next_s = np.asarray([max(t, aR[k]*(s1[k]-t)+t) for k, t in enumerate(target)])
    next_y = np.asarray([aA[k]*(y1[k]-v)+v for k, v in enumerate(next_s)])
    return 10**(-next_y/20), next_s, next_y, target


def aligned_key(key, main_channels, length):
    if key is None or len(key) == 0:
        raw = np.zeros((main_channels, length))
    elif len(key) == 1:
        raw = np.tile(np.asarray(key), (main_channels, 1))
    elif main_channels == 1:
        raw = np.mean(key, axis=0, keepdims=True)
    else:
        raw = np.asarray(key)
    return np.pad(raw, ((0, 0), (N, 0)))


def monitor_mix(dry, wet, key_aligned, events=None, fs=48000.,
                mix=.37, out_gain=.8, wet_gain=1.4, initial=(False, False, False)):
    """5 ms linear route-weight ramp, restarted from current weights on retarget.

    Route tuple is (bypass, sidechain_listen, delta). Precedence is in that
    order. Selection only changes monitoring; detector keeps running upstream.
    At the event sample the ramp advances its first 1/L step; Lth is exact end.
    """
    def select(flags):
        bypass, listen, delta = flags
        return 3 if bypass else 2 if listen else 1 if delta else 0
    routes = np.asarray([((1-mix)*dry+mix*wet*wet_gain)*out_gain,
                         (wet*wet_gain-dry)*out_gain,
                         key_aligned*out_gain, dry])
    weights = np.eye(4)[select(initial)]
    target, step = weights.copy(), np.zeros(4)
    remaining, ramp = 0, max(1, int(math.floor(.005*fs+.5)))
    output, history = np.zeros_like(dry), []
    for n in range(dry.shape[1]):
        if n in (events or {}):
            requested = np.eye(4)[select(events[n])]
            if not np.array_equal(requested, target):
                target = requested
                step, remaining = (target-weights)/ramp, ramp
        if remaining:
            weights += step
            remaining -= 1
            if remaining == 0:
                weights = target.copy()
        history.append(weights.copy())
        output[:, n] = np.sum(routes[:, :, n]*weights[:, None], axis=0)
    return output, np.asarray(history)


def probes(length=4096, fs=48000.):
    t = np.arange(length)/fs
    rng = np.random.default_rng(0x51DE)
    main = np.asarray([.21*np.sin(2*np.pi*3000*t)+.05*np.sin(2*np.pi*750*t),
                       .13*np.sin(2*np.pi*3000*t)-.08*np.sin(2*np.pi*750*t)])
    key = np.asarray([.28*np.sin(2*np.pi*3000*t)+.001*rng.standard_normal(length),
                      .07*np.sin(2*np.pi*750*t)+.001*rng.standard_normal(length)])
    return main, key


def test_legacy_and_chunks():
    main, key = probes()
    worst = chunk = 0.
    for p in (hw.Params(), hw.Params(ms_mode=True, link_pct=0, focus=.2)):
        for mode in (0, 1):
            old, _ = hw.render(main, p=p, mode=mode)
            off, _ = render(main, key, p, mode, enabled=False)
            worst = max(worst, float(np.max(np.abs(old-off))))
            events = {1280: (True, 1-mode), 2304: (False, mode), 3072: (True, mode)}
            connections = {1792: 0, 2560: 1, 3328: 2}
            a, _ = render(main, key, p, mode, events=events, connection_events=connections)
            b, _ = render(main, key, p, mode, events=events,
                          connection_events=connections, chunks=(99999,))
            chunk = max(chunk, float(np.max(np.abs(a-b))))
    assert worst == 0 and chunk == 0, (worst, chunk)
    return {'legacy_max_error': worst, 'chunk_max_error': chunk}


def test_oracle_and_carrier():
    main, key = probes(3072)
    # Nonzero noise floor keeps Soft relative ratios well-conditioned when a
    # direct O(N^2) DFT and an FFT differ by ~1e-12 at cancelled pure-tone bins.
    main += .002*np.random.default_rng(0xDF7).normal(size=main.shape)
    p = hw.Params(ms_mode=True, link_pct=33, focus=-.3, max_cut_db=9,
                  tilt_attack=.4, tilt_release=-.2, tilt_detail=.3)
    worst_dft = worst_gain = worst_audio = worst_linear = 0.
    n = np.arange(N)
    dft = np.exp(-2j*np.pi*np.arange(N//2+1)[:, None]*n/N)
    padded = np.pad(key, ((0, 0), (N-HOP, N)))
    for mode in (0, 1):
        events = {1280: (False, 1-mode), 1792: (True, 1-mode), 2560: (True, mode)}
        y, proc = render(main, key, p, mode, events=events)
        s = np.zeros((2, N//2+1)); z = s.copy()
        oracle_gains = []
        for frame in range(proc.hops):
            physical = padded[:, frame*HOP:frame*HOP+N]*proc.window
            spectrum = np.asarray([dft@lane for lane in physical])
            mid, side = (spectrum[0]+spectrum[1])/np.sqrt(2), (spectrum[0]-spectrum[1])/np.sqrt(2)
            mag = np.abs(np.asarray([mid, side]))
            enabled, active_mode = True, mode
            for at, controls in events.items():
                if at <= (frame+1)*HOP:
                    enabled, active_mode = controls
            if enabled:
                worst_dft = max(worst_dft, float(np.max(np.abs(mag-proc.detectors[frame]))))
            else:
                # Independently transform the main carrier for internal-source
                # frames; retained s/y states carry across every switch.
                main_padded = np.pad(main, ((0, 0), (N-HOP, N)))
                main_dft = dft@(main_padded[:, frame*HOP:frame*HOP+N]*proc.window).T
                mag = np.abs(np.asarray([(main_dft[:, 0]+main_dft[:, 1])/np.sqrt(2),
                                        (main_dft[:, 0]-main_dft[:, 1])/np.sqrt(2)]))
            linked = np.asarray([[p.link_pct/100*max(mag[0, k], mag[1, k])
                                 +(1-p.link_pct/100)*mag[c, k] for k in range(N//2+1)] for c in range(2)])
            gains = []
            for c in range(2):
                g, s[c], z[c], _ = scalar_gain_oracle(linked[c], proc.cores[c], active_mode, enabled, s[c], z[c])
                gains.append(g)
            oracle_gains.append(gains)
        worst_gain = max(worst_gain, float(np.max(np.abs(np.asarray(oracle_gains)-proc.gains))))
        reference = hw.scipy_wola_reference(main, 48000., oracle_gains, p)
        worst_audio = max(worst_audio, float(np.max(np.abs(y-reference))))
        fixed, fixed_proc = render(main, key, p, mode)
        y2, proc2 = render(-.37*main, key, p, mode)
        worst_linear = max(worst_linear, float(np.max(np.abs(y2+.37*fixed))))
        assert np.array_equal(fixed_proc.gains, proc2.gains)
        _, changed = render(main, key*.04, p, mode)
        # Hard responds to key LEVEL. Soft is nearly level-independent: change
        # its spectral shape instead when proving external detector influence.
        if mode == 0:
            _, changed = render(main, key[:, ::-1]+.05*np.random.default_rng(2).normal(size=key.shape), p, mode)
        assert np.max(np.abs(np.asarray(fixed_proc.gains)-changed.gains)) > .001
    assert worst_dft < 1e-10 and worst_gain < 1e-10 and worst_audio < 1e-10, (worst_dft, worst_gain, worst_audio)
    assert worst_linear < 1e-12
    return {'direct_dft_magnitude_error': worst_dft, 'scalar_gain_error': worst_gain,
            'scipy_audio_error': worst_audio, 'carrier_linearity_error': worst_linear}


def test_routing_release_and_warm_history():
    main, key = probes(4096)
    p = hw.Params(ms_mode=True, link_pct=0)
    worst_release = mono_lanes = phase = warm = 0.
    for mode in (0, 1):
        for missing in (None, np.zeros_like(key)):
            silent_key, _ = render(main, missing, p, mode)
            assert np.max(np.abs(silent_key-np.pad(main, ((0, 0), (N, 0))))) < 1e-12
        _, mono = render(main, key[:1], p, mode)
        mono_lanes = max(mono_lanes, float(np.max(np.abs(np.asarray(mono.gains)[:, 0]-np.asarray(mono.gains)[:, 1]))))
        for sign in (1, -1):
            stereo = np.asarray([key[0], sign*key[0]])
            _, carrier_mono = render(main[:1], stereo, p, mode)
            _, key_mono = render(main[:1], key[:1], p, mode)
            phase = max(phase, float(np.max(np.abs(np.asarray(carrier_mono.gains)-key_mono.gains))))
            _, ms = render(main, stereo, p, mode)
            # Pure M / pure S key leaves the unexcited detector lane at unity.
            assert np.max(np.abs(np.asarray(ms.gains)[:, 1 if sign == 1 else 0]-1)) == 0
        # Both silent-connected and disconnected histories converge to zero,
        # with no Soft baseline; verify every decay step from its actual state.
        for disconnected in (False, True):
            proc = SidechainWola(p=p, mode=mode)
            proc.process(main, key)
            for _ in range(20):
                previous_s = [c.ballistics.s1.copy() for c in proc.cores]
                previous_y = [c.ballistics.y1.copy() for c in proc.cores]
                proc.process(np.zeros((2, HOP)), None if disconnected else np.zeros((2, HOP)))
                # Connected silence retains earlier key within the N window.
                if disconnected or not np.any(proc.key_ring):
                    for lane, c in enumerate(proc.cores):
                        s = np.broadcast_to(c.ballistics.aR, previous_s[lane].shape)*previous_s[lane]
                        y = c.ballistics.aA*(previous_y[lane]-s)+s
                        worst_release = max(worst_release, float(np.max(np.abs(c.ballistics.y1-y))))
            assert np.max(proc.cores[0].ballistics.y1) < np.max(previous_y[0])
        # At hop1280 the first enabled key spectrum already contains 1024
        # connected key samples, although main detection was selected before.
        _, switched = render(main, key, p, mode, enabled=False, events={1280: (True, mode)})
        _, always = render(main, key, p, mode)
        warm = max(warm, float(np.max(np.abs(switched.detectors[4]-always.detectors[4]))))
    assert mono_lanes == 0 and phase < 1e-12 and worst_release < 1e-12 and warm == 0
    return {'mono_key_lane_error': mono_lanes, 'antiphase_power_downmix_error': phase,
            'release_recurrence_error': worst_release, 'disabled_key_history_error': warm}


def test_monitor():
    main, key = probes(3072)
    wet, _ = render(main, key, hw.Params(depth=0), mode=1)
    dry = np.pad(main, ((0, 0), (N, 0)))
    identity = float(np.max(np.abs(wet-dry)))
    key_delay = aligned_key(key, 2, main.shape[1])
    worst = 0.
    for flags, expected in (((False, True, True), key_delay*.8),
                            ((True, True, True), dry),
                            ((False, False, True), np.zeros_like(dry))):
        y, _ = monitor_mix(dry, wet, key_delay, initial=flags, wet_gain=1)
        worst = max(worst, float(np.max(np.abs(y-expected))))
    for mix, trim in ((0., .01), (1., 7.)):
        listen, _ = monitor_mix(dry, wet*.3, key_delay, mix=mix,
                                wet_gain=trim, initial=(False, True, False))
        worst = max(worst, float(np.max(np.abs(listen-key_delay*.8))))
    mono_key = aligned_key(key[:1], 2, main.shape[1])
    assert np.array_equal(mono_key[0], mono_key[1])
    assert np.array_equal(aligned_key(key, 1, main.shape[1])[0], np.mean(key_delay, axis=0))
    assert np.max(np.abs(aligned_key(None, 2, main.shape[1]))) == 0
    events = {1100: (False, True, True), 1120: (False, True, True), 1200: (True, True, True),
              1600: (False, False, True), 2000: (False, False, False)}
    y, weights = monitor_mix(dry, wet, key_delay, events=events)
    # Independent closed-form segment ramps, including interrupted SC fade.
    reference = np.zeros_like(y)
    routes = np.asarray([((1-.37)*dry+.37*wet*1.4)*.8,
                         (wet*1.4-dry)*.8, key_delay*.8, dry])
    start, w = 0, np.asarray([1., 0, 0, 0])
    ramp = 240
    fade_error = 0.
    for end, index in [(1100, 2), (1200, 3), (1600, 1), (2000, 0), (y.shape[1], None)]:
        for n in range(start, end):
            q = min(1., (n-start+1)/ramp) if start else 1.
            v = w if start == 0 else (1-q)*w+q*target
            reference[:, n] = sum(v[c]*routes[c, :, n] for c in range(4))
            fade_error = max(fade_error, float(np.max(np.abs(v-weights[n]))))
        if index is not None:
            w, target = v.copy(), np.eye(4)[index]
        start = end
    audio_error = float(np.max(np.abs(y-reference)))
    assert identity < 1e-12 and worst < 1e-12 and fade_error < 1e-12 and audio_error < 1e-12
    return {'identity_delay_error': identity, 'monitor_precedence_null_error': worst,
            'crossfade_weight_error': fade_error, 'crossfade_audio_error': audio_error}


def run_all(render_wavs=True):
    results = {}
    for number, (name, test) in enumerate([
            ('legacy and arbitrary chunks', test_legacy_and_chunks),
            ('independent DFT/control/SciPy and carrier', test_oracle_and_carrier),
            ('key routing/release/warmed history', test_routing_release_and_warm_history),
            ('independent monitor/alignment/crossfade', test_monitor)], 1):
        metrics = test()
        results[name] = metrics
        print(f'test {number} {name}: PASS {json.dumps(metrics, sort_keys=True)}')
    print('SUMMARY: four external-sidechain/current-WOLA gates passed')
    print('VERDICT: PASS (our approximation; no soothe sonic equivalence claim)')
    if render_wavs:
        main, key = probes(24000)
        soft, _ = render(main, key, mode=0)
        hard, _ = render(main, key, mode=1)
        output = Path(__file__).resolve().parent/'out'
        for name, audio in (('main', main), ('key', key), ('soft', soft[:, N:]), ('hard', hard[:, N:])):
            wavfile.write(output/f'spectral_sidechain_{name}.wav', 48000, audio.T.astype(np.float32))
    return results


if __name__ == '__main__':
    run_all()
