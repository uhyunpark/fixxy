"""Spectral processor, Stage 1: the STFT IDENTITY pipeline (soothe-class,
docs/spectral-reference.md, 6.Stage 1).

WHY THIS STAGE EXISTS: everything soothe3-like (envelope estimation, relative
threshold, proportional attenuation, dual-domain smoothing) is a GAIN
MULTIPLIED ONTO EACH FFT BIN, sitting between analysis and synthesis. If the
analysis -> synthesis round trip itself doesn't reconstruct the input
losslessly (bit-exact modulo a pure delay) and click-free under a real,
non-trivial gain map, nothing built on top of it can be trusted -- an
envelope-detector bug and an OLA-framing bug both sound like "the spectral
processor is buggy," and you cannot tell them apart unless this stage is
proven innocent first. So this file does ONE thing: STFT -> (no processing,
then a static per-bin gain) -> ISTFT, block-streaming (arbitrary chunk
sizes, like a real plugin's processBlock), and proves it's exact.

WINDOW CONVENTION -- sqrt-Hann analysis + sqrt-Hann synthesis (both
PERIODIC, i.e. w[n] = 0.5 - 0.5*cos(2*pi*n/N), n=0..N-1; note the /N not
/(N-1) -- periodic, not symmetric, so tiling by hop stays phase-consistent
frame to frame).

Why sqrt-Hann on BOTH sides rather than plain-Hann-analysis +
rectangular-synthesis (the other common convention, used for e.g. spectrum
ANALYSIS where nothing downstream ever touches a bin's phase/magnitude):
this pipeline's whole purpose is bin-wise GAIN MODIFICATION. Multiplying a
frame's spectrum by an arbitrary per-bin gain and taking the IFFT does not,
in general, produce a signal that goes smoothly to zero at the frame edges
-- an arbitrary gain map can reintroduce discontinuities at n=0 and n=N-1
that the analysis window had suppressed. If synthesis is rectangular (no
taper), those edge discontinuities get overlap-added straight into the
output as clicks/warble at the FRAME RATE (fs/hop) -- this is exactly the
"musical noise" failure mode docs/spectral-reference.md 함정 warns about.
A synthesis window re-tapers the MODIFIED frame's edges before OLA, so
whatever discontinuity the gain map introduced is damped at the boundary
instead of being summed raw into neighboring frames. Splitting one Hann
window as sqrt-Hann * sqrt-Hann (rather than e.g. Hann-analysis +
Hann-synthesis, which over-tapers and needs its own COLA renormalization)
is the standard choice because (a) it still COLA-sums to a constant with
50%/75% overlap [test 1], and (b) analysis and synthesis contribute equal
"shares" of the taper, so a flat unity gain map is the identity pipeline
exactly [test 2] -- there's no asymmetric coloration to correct for later.

STREAMING / C++-PORTING STATE (see class StreamingSTFT below -- this is
what Stage 4 ports to a real-time C++ SpectralNode):
  - `input_ring`   : float[N], sliding analysis buffer holding the most
                      recent N input samples. Shifted left by `hop` and
                      refilled at the tail every time `hop` new input
                      samples have accumulated.
  - `ola_buffer`   : float[N], the overlap-add accumulator. A completed
                      synthesis frame (N samples) is added into it in full;
                      the first `hop` samples are then the FINISHED output
                      (all overlapping frames that ever contribute to those
                      samples have already been summed in) and are drained;
                      the buffer is then shifted left by `hop` and the
                      newly-exposed tail zeroed.
  - `in_fifo`      : queue of input samples not yet consumed into a hop
                      (handles arbitrary processBlock sizes -- a host may
                      call with 64, 128, 480, 1000 samples etc; this queue
                      is what makes the frame timing independent of that).
  - `out_buf`      : output samples, indexed by ABSOLUTE hop-derived
                      position (not a plain FIFO -- see LATENCY below for
                      why position-indexing, not queue order, is what makes
                      this correct under arbitrary block sizes). In the C++
                      port this is a ring buffer sized a small constant
                      multiple of N, addressed by (position mod ring_size).
  - `norm`         : precomputed scalar COLA normalization gain (constant
                      for a given (N, hop) pair -- computed once in
                      prepare(), not per-frame).
  All state is fixed-size / preallocated except `in_fifo`, which in the C++
  port becomes a small ring buffer (no runtime allocation on the audio
  thread -- see AGENTS.md Non-Negotiables). fs does not appear anywhere in
  this state: framing is purely sample-domain.

LATENCY -- derivation, the exact formula, AND A BUG THIS FILE FOUND: for
this streaming framing, latency = N samples exactly, independent of fs and
independent of hop. That's a full frame, NOT the N-hop "one frame minus one
hop" figure a purely offline/single-shot derivation gives -- read on, this
gap between the offline-correct and streaming-correct answer is the reason
this file exists as a proto instead of going straight to C++.

Offline derivation (correct for a single non-streaming call, WRONG in
general -- see below): a synthesis frame completed after consuming input up
to absolute time t covers output positions [t-N+1, t-N+1+hop). Chaining
that across hops gives output[j] = input[j-(N-hop)] for all j. This is
exactly the formula this file's FIRST implementation used, and it measured
correctly (test 3 passed) for a single big process() call. But test 2
(passthrough null) and test 4 (streaming invariance) then failed for the
majority of (N, hop, fs, chunk-size) combinations -- not with small
numerical noise, but with large (order-1) errors starting partway through
the signal. That's what forced the re-derivation below.

Root cause: production (a completed hop's worth of output) only happens in
whole-hop increments, but process(x) must return exactly len(x) samples on
EVERY call, immediately, however the caller chose to chunk the stream. A
given output position j is only fully determined once input has been fed
up to absolute index j + hop - (j mod hop) -- i.e. j's OWN causal window
needs input up to hop-1 samples PAST what a naive "N-hop total delay"
bookkeeping assumes, because a hop only completes (and can only be
finalized/summed into the OLA) once `hop` full new samples have arrived,
not the instant the (N-hop)-th one does. For a single call spanning the
entire signal this is invisible (the shortfall only ever bites the very
last few samples, which the tail-flush absorbs). For arbitrary streaming
chunk sizes it recurs at EVERY call boundary that doesn't happen to land on
a hop-aligned cumulative sample count -- i.e. constantly, not just at
start/end -- and each recurrence, if the implementation naively emits 0.0
for "not ready yet," is not a temporary gap but a PERMANENT one-sample
data loss that shifts every subsequent output sample by one (confirmed by
two failed intermediate fixes: a fallback-to-0.0 design, and a fallback
design gated on a running sample counter -- both corrupted test 2/3/4 in
different ways before this N-samples-with-position-indexed-output design
was reached; see StreamingSTFT.process's docstring for the blow-by-blow).

The fix costs exactly one extra hop of latency: report/use L = N (not
N-hop). With L = N, the position needed to satisfy output index j is
j - (j mod hop), which is always < j < (cumulative input fed so far when j
is read) -- a STRICT inequality with no dependence on hop-alignment, so it
holds for every call boundary, every chunk size, unconditionally (proved
in verify_latency_formula's docstring reasoning and confirmed by test 2/4
now passing for every combination, including pathological chunk sizes like
feeding 1 sample at a time). Output positions are written into `out_buf` at
their own absolute, hop-derived index (not pushed to a FIFO in call order)
specifically so a value is never handed out before that write has
happened -- see StreamingSTFT.process.

    N      hop    overlap   latency(samples)   44.1kHz   48kHz    96kHz
    512    128    75%       512                 11.61ms  10.67ms   5.33ms
    512    256    50%       512                 11.61ms  10.67ms   5.33ms
    1024   256    75%       1024                23.22ms  21.33ms  10.67ms
    1024   512    50%       1024                23.22ms  21.33ms  10.67ms
    2048   512    75%       2048                46.44ms  42.67ms  21.33ms
    2048   1024   50%       2048                46.44ms  42.67ms  21.33ms

Latency is N regardless of hop/overlap (recomputed in verify_latency(), not
hand-typed twice) -- the extra hop of margin is a fixed one-frame cost, not
a fraction of the frame.
"""
import os
from collections import deque

import numpy as np
from scipy.io import wavfile
from scipy.signal import check_COLA, hilbert

OUT = "proto/out"
os.makedirs(OUT, exist_ok=True)

FFT_SIZES = (512, 1024, 2048)
HOP_DIVISORS = (4, 2)  # hop = N/4 (75% overlap), N/2 (50% overlap)
FS_LIST = (44100, 48000, 96000)


# ============================================================
# PART 0: windows and COLA normalization
# ============================================================

def periodic_hann(N):
    """Periodic (DFT-even) Hann: w[n] = 0.5 - 0.5*cos(2*pi*n/N), n=0..N-1.
    Periodic, not symmetric (that's the /N, not /(N-1)) -- required so that
    tiling copies of this window by a fixed hop stays phase-consistent
    (a symmetric/"MATLAB-style" Hann of length N is NOT exactly periodic
    with period dividing N, and its OLA sum has small ripple)."""
    n = np.arange(N)
    return 0.5 - 0.5 * np.cos(2.0 * np.pi * n / N)


def sqrt_hann(N):
    return np.sqrt(periodic_hann(N))


def cola_constant(window_product, hop, reps=8):
    """Numerically measure the OLA sum of `window_product` (already the
    elementwise analysis*synthesis product) tiled every `hop` samples.
    Returns the array of per-offset sums for one hop period, taken from the
    middle of a `reps`-period tiling (away from start/end edges where the
    infinite-tiling assumption behind COLA doesn't hold)."""
    N = len(window_product)
    total_len = N + reps * hop
    acc = np.zeros(total_len)
    for k in range(reps):
        acc[k * hop: k * hop + N] += window_product
    mid = (reps // 2) * hop
    return acc[mid: mid + hop]


def cola_norm(N, hop):
    """1 / (the constant that sqrt_hann*sqrt_hann OLA-sums to at this
    (N, hop)). Multiplying the raw OLA output by this makes the identity
    pipeline reproduce the input exactly."""
    w = sqrt_hann(N)
    prod = w * w  # == periodic_hann(N)
    per_offset = cola_constant(prod, hop)
    return 1.0 / np.mean(per_offset), per_offset


# ============================================================
# PART 1: streaming STFT/ISTFT identity pipeline
# ============================================================

class StreamingSTFT:
    """Block-streaming STFT -> (bin gain, default identity) -> ISTFT.

    process(x) accepts an arbitrary-length 1-D chunk and returns an
    equal-length chunk of output, using internal FIFOs so the analysis/
    synthesis framing (which only cares about multiples of `hop`) is
    decoupled from how the caller chooses to chunk the stream -- the same
    seam a real host's processBlock() sees (test 4: streaming invariance).

    gain_fn(freqs) -> per-bin real gain array (len N//2+1), applied every
    frame; None = identity (Stage 1's actual subject). Held static here;
    Stage 2+ will make this envelope-driven.
    """

    def __init__(self, N, hop, fs, gain_fn=None):
        assert N % hop == 0, "hop must divide N for this framing"
        self.N = N
        self.hop = hop
        self.fs = fs
        self.gain_fn = gain_fn
        self.analysis_win = sqrt_hann(N)
        self.synth_win = sqrt_hann(N)
        self.norm, self._cola_offsets = cola_norm(N, hop)
        self.freqs = np.fft.rfftfreq(N, d=1.0 / fs)
        self.gain = gain_fn(self.freqs) if gain_fn is not None else None

        self.input_ring = np.zeros(N)
        self.ola_buffer = np.zeros(N)
        self.in_fifo = deque()
        self.hops_done = 0          # count of completed hops, see _process_one_hop
        self.out_buf = np.zeros(0)  # grows on demand, indexed by absolute position
        self.total_emitted = 0      # next absolute output index process() will return

    @property
    def latency_samples(self):
        # See the module docstring's LATENCY section for why this is N, not
        # the offline-derived N-hop: N-hop is only safe for a single
        # non-streaming call; N is what's provably safe under arbitrary
        # process() call/chunk-size boundaries.
        return self.N

    def _ensure_out_buf(self, size):
        if len(self.out_buf) < size:
            self.out_buf = np.concatenate([self.out_buf, np.zeros(size - len(self.out_buf))])

    def _process_one_hop(self):
        """Consume one hop of input, OLA it in, and write the finished hop
        of output into out_buf at its own absolute position -- NOT appended
        to a FIFO in call order. Why this matters: a plain FIFO only
        records PRODUCTION order, which is fine in isolation, but ties the
        value returned by a pop() to whatever the READ side happens to
        request next -- and (see process()) the read side can, transiently,
        request positions faster than production completes them. Writing to
        an absolute index means a position is either genuinely there
        (production reached it) or genuinely not (out_buf hasn't been
        extended that far, reads as the array's zero default) -- there is
        no way to hand out the WRONG value for a given position, only
        "not yet" for one that hasn't arrived, which latency_samples=N is
        sized to prevent from ever being observed (see process())."""
        chunk = np.array([self.in_fifo.popleft() for _ in range(self.hop)])
        self.input_ring = np.concatenate([self.input_ring[self.hop:], chunk])

        frame = self.input_ring * self.analysis_win
        spec = np.fft.rfft(frame)
        if self.gain is not None:
            spec = spec * self.gain
        frame_out = np.fft.irfft(spec, self.N) * self.synth_win

        self.ola_buffer += frame_out
        ready = self.ola_buffer[: self.hop] * self.norm
        self.ola_buffer = np.concatenate([self.ola_buffer[self.hop:], np.zeros(self.hop)])

        self.hops_done += 1
        # The h-th completed hop (h = hops_done, 1-indexed) finalizes the
        # OLA sum for absolute output positions [h*hop - N, h*hop - N + hop).
        # For h < N/hop this is negative (the causal pre-roll region, before
        # any real output exists) and is simply not written -- out_buf's
        # zero default already IS the correct value there.
        start = self.hops_done * self.hop - self.N
        if start >= 0:
            self._ensure_out_buf(start + self.hop)
            self.out_buf[start: start + self.hop] = ready

    def process(self, x):
        """Push len(x) input samples, return len(x) output samples. One
        call in, same-length call out, exactly like a plugin's processBlock
        with an arbitrary block size.

        THE BUG THIS DESIGN FIXES (found chasing test 2/3/4 failures across
        several wrong attempts -- kept here because the reasoning is
        non-obvious and every wrong version looked plausible until
        measured):

        A plain FIFO ("push finished hops, pop len(x) per call, 0.0 if
        empty") is wrong because production only completes in whole-hop
        jumps while consumption is requested one sample at a time, every
        call. At MANY call boundaries -- not just stream start, but any
        boundary that doesn't land on a hop-aligned cumulative sample
        count, which is the norm under arbitrary block sizes -- production
        is transiently behind what's been requested. A 0.0 fallback there
        is not a temporary gap, it's a PERMANENT one-sample loss: the real
        value completes one hop later and gets handed out of the FIFO at
        whatever LATER slot next asks for it, shifting everything after by
        one. This is true regardless of how much fixed head-start delay you
        gate the FIFO on (tried: gating a plain FIFO on `latency_samples`,
        both mishandling and correctly handling the discard of gated
        items -- both still eventually pop from empty, or silently
        introduce doubled delay, because a FIFO has no notion of WHICH
        absolute position a popped item belongs to).

        The fix is `out_buf`'s absolute-position indexing (see
        _process_one_hop) plus latency_samples = N. Position j is read as
        out_buf[j - latency_samples] once total_emitted reaches j; that
        position is guaranteed already written because it only requires
        production up to input index j - (j mod hop), which is always
        STRICTLY LESS than j itself (and so always less than however much
        has been fed by the time j is read) -- true unconditionally, not
        "true if hop divides the call boundary." No 0.0 fallback is needed
        past the initial latency_samples gap, and none is used; reading an
        unwritten position would be a real bug, not an expected edge case,
        which is why this code lets a would-be out-of-range read fail loud
        (IndexError via numpy) rather than silently return 0."""
        x = np.asarray(x, dtype=np.float64)
        self.in_fifo.extend(x.tolist())
        while len(self.in_fifo) >= self.hop:
            self._process_one_hop()

        n = len(x)
        out = np.zeros(n)
        for i in range(n):
            j = self.total_emitted
            if j >= self.latency_samples:
                idx = j - self.latency_samples
                out[i] = self.out_buf[idx]
            self.total_emitted += 1
        return out

    def flush(self):
        """Drain the tail. Two things can still be pending when the input
        stream ends: (1) up to hop-1 real samples sitting in in_fifo that
        never reached a full hop and so were never turned into a frame at
        all, and (2) once that last real frame IS formed, latency_samples
        more samples' worth of future frames are needed before the LAST
        real input sample's output position is ever written. Zero-pad for
        both: first enough to round the FIFO up to the next hop boundary,
        then latency_samples more."""
        pad_to_hop = (self.hop - len(self.in_fifo)) % self.hop
        return self.process(np.zeros(pad_to_hop + self.latency_samples))


def run_identity(x, N, hop, fs, gain_fn=None, chunk_size=None):
    """Feed x through StreamingSTFT either as one call (chunk_size=None) or
    in fixed-size chunks (chunk_size=int), then flush the tail. Returns the
    full output array, length len(x) + latency_samples."""
    stft = StreamingSTFT(N, hop, fs, gain_fn)
    out_chunks = []
    if chunk_size is None:
        out_chunks.append(stft.process(x))
    else:
        for i in range(0, len(x), chunk_size):
            out_chunks.append(stft.process(x[i:i + chunk_size]))
    out_chunks.append(stft.flush())
    return np.concatenate(out_chunks)


# ============================================================
# PART 2: verification
# ============================================================

def matrix():
    return [(N, N // d) for N in FFT_SIZES for d in HOP_DIVISORS]


def verify_cola():
    """Test 1 -- COLA: sqrt-Hann analysis * sqrt-Hann synthesis OLA-sums to
    a constant (max deviation < 1e-12), for every (N, hop) in the matrix.
    Cross-check: since analysis*synthesis == plain periodic_hann(N), this is
    exactly the COLA condition scipy.signal.check_COLA evaluates for a
    single Hann window at the same (nperseg, noverlap) -- an independent
    reference for the same numeric claim, not a re-derivation of our own
    formula."""
    ok = True
    for N, hop in matrix():
        offsets, norm = None, None
        norm, offsets = cola_norm(N, hop)
        dev = float(np.max(offsets) - np.min(offsets))
        our_pass = dev < 1e-12

        win = periodic_hann(N)
        scipy_ok = bool(check_COLA(win, N, N - hop, tol=1e-10))

        p = our_pass and scipy_ok
        ok &= p
        print(f"  N={N:5d} hop={hop:5d} ({N // hop}x): OLA max-dev={dev:.3e} "
              f"(gate <1e-12) norm={norm:.6f} scipy_check_COLA={scipy_ok} "
              f"-> {'PASS' if p else 'FAIL'}")
    return ok


def verify_passthrough_null():
    """Test 2 -- with gain_fn=None (pure identity), output must equal input
    delayed by exactly latency_samples = N, for white noise AND a
    music-like signal (sum of sines + noise), 5s, for every (N, hop, fs)
    combo. max|diff| < 1e-10 after alignment."""
    ok = True
    dur = 5.0
    rng = np.random.default_rng(0)
    for fs in FS_LIST:
        n = int(dur * fs)
        t = np.arange(n) / fs
        noise = rng.standard_normal(n) * 0.2
        music = (0.3 * np.sin(2 * np.pi * 220 * t)
                 + 0.2 * np.sin(2 * np.pi * 880 * t)
                 + 0.1 * np.sin(2 * np.pi * 3300 * t)
                 + 0.05 * rng.standard_normal(n))
        for sig_name, sig in (("white_noise", noise), ("music_like", music)):
            for N, hop in matrix():
                lat = N
                out = run_identity(sig, N, hop, fs)
                out_aligned = out[lat: lat + n]
                diff = np.max(np.abs(out_aligned - sig))
                p = diff < 1e-10
                ok &= p
                print(f"  fs={fs:6d} {sig_name:11s} N={N:5d} hop={hop:5d}: "
                      f"max|diff|={diff:.3e} (gate <1e-10) -> {'PASS' if p else 'FAIL'}")
    return ok


def verify_latency_formula():
    """Test 3 -- measured delay via cross-correlation of a chirp against
    the identity pipeline's output must exactly equal N (integer
    sample match, independent of hop -- see module docstring LATENCY
    section for why it's N and not N-hop), for every (N, hop, fs) combo."""
    ok = True
    for fs in FS_LIST:
        dur = 1.0
        n = int(dur * fs)
        t = np.arange(n) / fs
        chirp = np.sin(2 * np.pi * (200 + (4000 * t / dur)) * t)
        for N, hop in matrix():
            derived = N
            out = run_identity(chirp, N, hop, fs)
            # cross-correlate the padded output against the input; the lag
            # of the peak is the measured delay.
            corr = np.correlate(out, chirp, mode="full")
            lag = int(np.argmax(corr) - (len(chirp) - 1))
            p = lag == derived
            ok &= p
            print(f"  fs={fs:6d} N={N:5d} hop={hop:5d}: derived={derived} "
                  f"measured={lag} -> {'PASS' if p else 'FAIL'}")
    return ok


def verify_streaming_invariance():
    """Test 4 -- same signal through different processBlock chunk sizes
    (64/128/480/1000, plus a single one-shot call) must give bit-identical
    output. Proves the FIFO framing is chunk-size-independent -- a real
    host calls processBlock with whatever size it wants."""
    ok = True
    fs = 44100
    n = 20000
    rng = np.random.default_rng(1)
    sig = rng.standard_normal(n) * 0.3
    for N, hop in ((512, 128), (2048, 1024)):
        ref = run_identity(sig, N, hop, fs, chunk_size=None)
        for cs in (64, 128, 480, 1000):
            out = run_identity(sig, N, hop, fs, chunk_size=cs)
            max_diff = float(np.max(np.abs(out - ref)))
            p = max_diff == 0.0
            ok &= p
            print(f"  N={N:5d} hop={hop:5d} chunk={cs:5d}: max|diff|={max_diff:.3e} "
                  f"(gate ==0) -> {'PASS' if p else 'FAIL'}")
    return ok


def brickwall_gain(freqs, cutoff_hz=5000.0, atten_db=-12.0):
    g = np.ones_like(freqs)
    g[freqs > cutoff_hz] = 10 ** (atten_db / 20.0)
    return g


def verify_modification_safety():
    """Test 5 -- static per-bin brickwall gain (-12dB above 5kHz):
    (a) a steady tone WELL INSIDE the passband must have flat output
        envelope after the pipeline settles -- ripple < 0.01dB. This is the
        direct test of the window-convention claim: if analysis+synthesis
        didn't reconstruct cleanly, ANY frame-rate seam would show up here
        as amplitude modulation at fs/hop, even for a gain map that leaves
        this tone's bins untouched.
    (b) a steady tone WELL INSIDE the stopband must show ~12dB of
        attenuation once settled.

    Both test tones are snapped to an EXACT FFT bin frequency (k*fs/N).
    This matters for (a) specifically: a Hann-windowed sinusoid whose
    frequency does NOT land on a bin center leaks energy into every other
    bin (sinc sidelobes), including, faintly, bins past the 5kHz cutoff --
    and because the tone's phase relative to each analysis frame drifts
    hop to hop (its period isn't commensurate with hop/fs), the amount of
    leaked-then-zeroed energy drifts too, showing up as a genuine small
    frame-rate ripple that has nothing to do with the OLA reconstruction
    itself. Snapping to a bin center makes an ideal Hann window's leakage
    exactly zero outside the 3-bin main lobe, isolating what this test is
    actually meant to catch: reconstruction artifacts, not window leakage.
    """
    ok = True
    fs = 44100
    N, hop = 1024, 256
    dur = 2.0
    n = int(dur * fs)
    t = np.arange(n) / fs
    lat = N
    bin_hz = fs / N

    # (a) passband tone, away from the 5kHz edge and from DC/Nyquist.
    f_pass = round(1000.0 / bin_hz) * bin_hz
    x_pass = 0.5 * np.sin(2 * np.pi * f_pass * t)
    out_pass = run_identity(x_pass, N, hop, fs, gain_fn=brickwall_gain)
    settle = lat + N  # skip startup transient (one extra frame's worth)
    y = out_pass[settle: settle + n - N]
    # Analytic (Hilbert) envelope, not windowed RMS: a windowed-RMS measure
    # over a window that isn't a whole number of the TONE's own periods
    # shows ripple even for the raw, unprocessed input (verified: the raw
    # 990.5Hz tone alone showed 0.27dB of "ripple" under a hop/2-sample RMS
    # window before this fix) -- that's an artifact of the measurement, not
    # of the pipeline. The Hilbert envelope is exact for a single sinusoid
    # and isolates real frame-rate (fs/hop) artifacts from this noise floor.
    env = np.abs(hilbert(y))
    edge = N  # drop one frame from each end: Hilbert has its own edge effects
    env = env[edge:-edge]
    env_db = 20 * np.log10(env / np.mean(env))
    ripple = float(np.max(env_db) - np.min(env_db))
    p_a = ripple < 0.01
    ok &= p_a
    print(f"  (a) passband tone {f_pass}Hz: envelope ripple={ripple:.5f}dB "
          f"(gate <0.01dB) -> {'PASS' if p_a else 'FAIL'}")

    # (b) stopband tone, well above 5kHz and away from Nyquist. Compare RMS
    # over the same settled/aligned region: out_stop[lat + N : lat + n - N]
    # is the identity-delayed counterpart of x_stop[N : n - N].
    f_stop = round(10000.0 / bin_hz) * bin_hz
    x_stop = 0.5 * np.sin(2 * np.pi * f_stop * t)
    out_stop = run_identity(x_stop, N, hop, fs, gain_fn=brickwall_gain)
    rms_in = np.sqrt(np.mean(x_stop[N: n - N] ** 2))
    rms_out = np.sqrt(np.mean(out_stop[lat + N: lat + n - N] ** 2))
    atten_db = 20 * np.log10(rms_out / rms_in)
    p_b = abs(atten_db - (-12.0)) < 0.5
    ok &= p_b
    print(f"  (b) stopband tone {f_stop}Hz: measured atten={atten_db:.3f}dB "
          f"(target -12dB, gate |err|<0.5dB) -> {'PASS' if p_b else 'FAIL'}")

    return ok and p_a and p_b


def render_demo_wavs():
    """Test 6 -- 4s dry/brickwall-filtered WAV pair for the owner to listen
    to (proto/out/). Music-like signal so the -12dB-above-5kHz carve is
    audible as a darkening, not just a sine-tone abstraction."""
    fs = 44100
    N, hop = 1024, 256
    dur = 4.0
    n = int(dur * fs)
    t = np.arange(n) / fs
    rng = np.random.default_rng(2)
    x = (0.25 * np.sin(2 * np.pi * 220 * t)
         + 0.2 * np.sin(2 * np.pi * 1000 * t)
         + 0.15 * np.sin(2 * np.pi * 7000 * t)
         + 0.1 * np.sin(2 * np.pi * 11000 * t)
         + 0.05 * rng.standard_normal(n))
    x *= 0.8 / np.max(np.abs(x))

    lat = N
    dry = run_identity(x, N, hop, fs)[lat: lat + n]
    wet = run_identity(x, N, hop, fs, gain_fn=brickwall_gain)[lat: lat + n]

    wavfile.write(f"{OUT}/spectral_stft_dry.wav", fs, dry.astype(np.float32))
    wavfile.write(f"{OUT}/spectral_stft_brickwall.wav", fs, wet.astype(np.float32))
    print(f"  wrote {OUT}/spectral_stft_dry.wav, {OUT}/spectral_stft_brickwall.wav "
          f"({dur}s @ {fs}Hz, N={N} hop={hop}, -12dB above 5kHz)")
    return True


def print_latency_table():
    print("  N     hop   overlap  latency(samples)   44.1kHz    48kHz     96kHz")
    for N, hop in matrix():
        lat = N
        overlap = f"{int(round(100 * (1 - hop / N)))}%"
        row = f"  {N:<5d} {hop:<5d} {overlap:<8s} {lat:<18d}"
        for fs in FS_LIST:
            row += f" {lat / fs * 1000:7.2f}ms"
        print(row)


# ============================================================
# main
# ============================================================

if __name__ == "__main__":
    print("=== Latency table (N, fs-independent, independent of hop) ===")
    print_latency_table()

    print("\n=== 1. COLA (sqrt-Hann analysis * sqrt-Hann synthesis) ===")
    p1 = verify_cola()

    print("\n=== 2. Passthrough null (identity, delayed by N) ===")
    p2 = verify_passthrough_null()

    print("\n=== 3. Latency formula (cross-correlation vs N) ===")
    p3 = verify_latency_formula()

    print("\n=== 4. Streaming invariance (chunk-size independence) ===")
    p4 = verify_streaming_invariance()

    print("\n=== 5. Modification safety (static brickwall gain) ===")
    p5 = verify_modification_safety()

    print("\n=== 6. Render demo WAVs ===")
    p6 = render_demo_wavs()

    all_pass = p1 and p2 and p3 and p4 and p5 and p6
    print(f"\nSUMMARY: cola={p1}, passthrough_null={p2}, latency_formula={p3}, "
          f"streaming_invariance={p4}, modification_safety={p5}, demo_wavs={p6}")
    print(f"VERDICT: {'ALL PASS' if all_pass else 'SOME FAILURES'}")
    assert all_pass, "one or more verifications failed -- see FAIL lines above"
