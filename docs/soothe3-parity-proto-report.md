# Soothe3 parity Stage 2 prototype report

Date: 2026-09-10. Status: bounded static renderer, synthesis, control-map, and
mode/sample-rate gates pass. The literal two-endpoint transition is rejected and
the full dynamic FIR engine remains unapproved pending a cadence-preserving
redesign. This is our designed approximation and does not prove soothe sonic
equivalence.

## Portable renderer handoff

The implementation is `proto/spectral_parity.py`. Its current fixed capacities
are `M=16384`, minimum-phase `L=4096`, `P=64`, `J=64`, and `J+1=65` retained
input spectra per channel. Every tail FFT is `2P=128`; `rfft`/`irfft` use NumPy's
unnormalized-forward, `1/N`-normalized-inverse convention. The first partition is
evaluated as a causal 64-tap direct sum. Tail partition `j` stores
`rfft([h[jP:(j+1)P], zeros(P)])`; internal block `b` evaluates only `X[b-j]`,
then adds the prior block's second overlap half. Internal boundaries do not
depend on host callback boundaries. The convolver contributes zero samples of
buffer delay.

Measured renderer maxima (float64, normalized random and boundary-tap fixtures):

- SciPy `fftconvolve`: `1.38777878078e-16`
- independent literal direct sum: `1.66533453694e-16`
- impulses at all 64 offsets, checked with SciPy `lfilter`: `4.16333634234e-17`
- arbitrary host partition sequence: `1.38777878078e-16`

All are below the required `1e-10`. The fixture explicitly has nonzero taps at
`P-1`, `P`, `2P-1`, `2P`, and `L-1`, and flushes the complete tail.

The primitive-only bridge is `proto/dump_spectral_fir_fixtures.py`, which
regenerates `test/spectral_fir_fixtures.h` only after rerunning the two static
gates. The header is deliberately named for FIR primitives and contains no
transition/dynamic-engine fixture. It pins a compact boundary-indexing
input/tap/output vector, one full `M/2+1=8193`-bin target curve and its 4096
minimum-phase taps, plus real/imaginary spectra for the first two partitions.
Its interface constants state `P`, `J`, `J+1`, FFT length, linear delay/taps,
sample rate, cap, and verified maxima. Spectrum arrays use bins `k=0..P`,
NumPy-compatible unnormalized forward RFFT and inverse scaling by `1/(2P)`.

## FIR synthesis

The real-cepstral design uses natural log, an explicitly mirrored real-even log
magnitude, and the specified causal-cepstrum lifter. It neither normalizes nor
windows the truncated result; unity is exactly `[1, 0, ...]`. Across 24 and
40 dB narrow 54 Hz targets, a broad 500 Hz target, harmonic curve, and low/high
shelves at 48 kHz:

- full design-grid relative magnitude error: `1.52655665886e-14`
- worst dense-grid truncated response error: `0.0809181453607 dB`
- worst attenuation-only above-unity ripple: `6.73856531961e-07 dB`
- worst endpoint cap overshoot relative to each suite target maximum:
  `0.00115059180955 dB`
- worst omitted-tail energy fraction: `5.7594299578e-16`
- periodic log-response Hilbert-relation error: `1.83186799063e-15 rad`
- independent NumPy/SciPy zero-convention error: `0`; small-design maximum zero
  radius: `0.873314248518`
- SciPy homomorphic spectral-factor sample error: `3.11111104656e-08`

The pinned SciPy 1.13.1 does not expose the newer `half=False` argument. Its
fixed-`n_fft=16384` half-length homomorphic result is therefore checked on a
known attenuation-only autocorrelation factor. The earlier failing root test was
a test convention bug: `np.roots(h[::-1])` measures reciprocal zeros for
`H(z)=sum h[n]z^-n`; `np.roots(h)` agrees exactly with `scipy.signal.tf2zpk`.

Linear phase uses `Llin=4097`, `D=2048`. Its frequency-sampling construction
differs from `scipy.signal.firwin2(..., window=None, nfreqs=8193)` by
`7.957523529e-14` per tap; symmetry error is `6.93889390391e-18`.

`M=32768` was initially selected for generous interpolation headroom on the
54 Hz notch. An explicit capacity reduction compared identical suites without
changing tolerances. `M=8192, L=4096` failed the 40 dB/54 Hz target at
`0.317410809588 dB` dense error. `M=16384, L=4096` passed at
`0.080918145361 dB` and is the smallest tested passing grid. Four-rate tests
preserve seconds of capacity: `(M,L)` is `(16384,4096)` at 44.1/48 kHz,
`(32768,8192)` at 96 kHz, and `(65536,16384)` at 192 kHz. Worst four-rate
dense error is `0.0808605539265 dB`, ripple `6.73515892953e-07 dB`.

## Transition rejection and gate

The direct time-varying raised-cosine oracle agrees with two independently run
partitioned banks to `5.55111512313e-17`. This establishes history/indexing math,
not safety of the chosen interpolation path.

The endpoint-only crossfade is **rejected**. At 48 kHz, transitioning from a
40 dB high-frequency shelf around 1 kHz to a 40 dB low-frequency shelf around
10 kHz with `M=16384`, `L=4096`, and `F=256` produced a measured cap overshoot
of `50.3801383123 dB` near `1428.93327433 Hz` at blend `t=0.708214780049`.
Thus the actual temporary reduction was about 90.38 dB despite both endpoints
being capped at 40 dB. Earlier lower-resolution probing found the same failure
class (66.16 dB overshoot). Thresholds were not relaxed.

As feasibility evidence only, five equal dB-curve/minimum-phase segments admitted
on `P=64` boundaries reduce the counterexample to `0.0284255554 dB` cap
overshoot and `2.8929824e-14 dB` ripple, agreeing with its direct time-varying
oracle to `1.45716772e-16`. It takes 320 samples (`6.667 ms` at 48 kHz) and six
IR endpoints. That exceeds High's `H=128` and Ultra's `H=64`, coalescing updates
into Normal-like cadence, so it is **not selected or exported**. The 1 ms Soft
detector constant is an envelope-pole label; whole-system reaction additionally
depends on analysis-window timing, hop, reduction ballistics, and FIR transition.
Even with that distinction, losing actual High/Ultra admission cadence is not
acceptable. Stage 1 subsequently redesigned this policy; the separate verified
result is recorded in `docs/soothe3-transition-proto-report.md`.

## Verified control handoff and prerequisites

`test/spectral_control_fixtures.h` is regenerated by
`proto/dump_spectral_control_fixtures.py`. It contains only control maps,
routing/mask identities, cap smoothing, and sample-rate capacity data—no FIR
transition or full-engine output.

Measured control maxima/minima:

- variable-width direct loop versus prefix sum: `1.72306613422e-13`
- link, focus, and mask endpoint/identity error: `0`
- M/S round-trip: `4.4408920985e-16`; energy error `1.13686837722e-13`
- Soft 36 dB level-scale spread: `1.11876426345e-07 dB`
- Soft detector closed-form pole error: `0`
- decoupled attack/release versus isolated SciPy/closed-form poles:
  `1.7763568394e-15`
- periodic-Hann on-bin/DC/Nyquist amplitude calibration:
  `2.77555756156e-17`
- automated cap versus old deep envelope state: `0` violation

The shelf coordinates, Detail width/strength/tau maps, normalized timing maps,
focus factors, linking, and masks depend on physical bin frequencies, not on a
specific FFT size or window. They are portable once their inputs have the stated
meaning. Detector calibration has stricter prerequisites:

- Hard consumes peak-amplitude-referred one-sided spectra. For any retained WOLA
  analysis window use `2*abs(X)/sum(w_analysis)` at positive interior bins and
  omit the factor two at DC/Nyquist. A COLA synthesis normalization does not
  replace this analysis-amplitude calibration. The verified periodic-Hann result
  cannot simply be assumed for a square-root Hann or other existing window.
- Soft's exact background convention is the median of **linear amplitudes** in
  the destination bin's local `+/-0.5 octave` interval; DC passes itself. The
  existing log-resampled approximate median is a different detector and needs
  a new comparison if retained.
- Soft is algebraically level-relative apart from its explicit epsilon floor,
  but the frozen knee constants are not automatically analysis-independent.
  FFT size/window change coherent-tone concentration, noise-bin statistics and
  off-bin leakage, so an N=1024 WOLA integration must rerun final-audio transfer
  calibration rather than borrow the proposed N=4096 periodic-Hann tuning.
- `tau_det(f)=0.001*2^(d(f)/2)` smooths Soft exceedance at frame cadence.
  Reduction state then uses the separate two-state decoupled attack/release law
  (`s1=max(x,aR*(s1-x)+x)` then `y=aA*(y1-s1)+s1`). This differs from a single
  branching pole and must not be silently mapped onto legacy envelope state.
- Silence or a missing external sidechain supplies zero new demand and releases
  existing reduction through that state; it must not fall back to main input.

Mode identity tests give exact null (`0`) and exact reported impulse latency for
minimum phase `B=2048`, linear phase `B+D=4096`, and low latency `B=0`. These are
formula/renderer checks, not host-notification integration tests.

Three explicitly synthetic unclipped WAVs were generated in `proto/out/`:
`spectral_parity_synthetic_dry.wav`, `spectral_parity_synthetic_minimum.wav`, and
`spectral_parity_synthetic_linear_raw.wav`. They are evidence for listening,
not a listening verdict or reference-plugin comparison.

## Bounded handoff verdict

Static FIR primitives, pure controls, timing-state formulas, masks, mode latency,
and the revised phase-limited transition math are verified with scoped fixtures.
Full dynamic FIR-engine integration remains unapproved pending separate C++
complete-path timing/audio-thread gates; reference-host sonic calibration and
product integration also remain separate work. No C++ production or public
schema was edited, and nothing was committed.
