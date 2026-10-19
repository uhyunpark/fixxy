# Spectral behavior specification — research stage

Date: 2026-09-09. Scope: stereo DSP and monitoring, not proprietary algorithm
reproduction. This is Stage 1 of `.agents/skills/dsp-pipeline/SKILL.md`. Every new
equation below remains a **candidate until the independent Python checks pass**.
No C++ implementation is authorized by the existence of this document alone;
the user has authorized implementation, but the prototype gate still applies.

Implementation handoff precedence: the later verified
`soothe3-parity-proto-report.md` and `soothe3-transition-proto-report.md`
supersede the initial capacities and transition/cap candidate below. In
particular, M=16384/L=4096 at 44.1/48 kHz, the fixed 0.01 rad admission reserve,
and the admitted effective-cap trajectory are the numerically tested choices.
The independent 20 ms requested-cap smoother alone is not an instantaneous
audio cap guarantee. Production integration and complete-path CPU gates remain
separate from those numerical results.

## Evidence boundary

**CONFIRMED public behavior** (compact paraphrase, 2026-09-09 retrieval):
Soft is adaptive and level-independent; Hard responds to absolute level, with
Depth changing its threshold. Detail increases notch precision and, in Soft,
slows detection. Timing varies by frequency. Tilt has independent low/high
controls. Full linking uses summed analysis; its summation domain is unspecified.
Bands guide suppression; bandpasses operate in parallel. Bands retain bypass
state, have channel focus, and support delta or solo listening. External SC drives
analysis. Max cut bounds reduction. Wet trim precedes Mix; Output follows it.
The graph reflects Mix. Default processing is minimum-phase. Quality primarily
changes time resolution; low-latency forces Normal/minimum-phase. At 44.1/48 kHz,
reported latencies are 2304 samples normally, 3456 in linear-phase, zero in
low-latency. Low-latency sacrifices lookahead.
[Official manual](https://oeksound.com/manuals/soothe3/).

Everything from here onward is **OUR DESIGNED APPROXIMATION / TEST REQUIREMENT**,
unless marked as an existing repository measurement. Neither a median detector
nor a tanh gain law is established as soothe's implementation. The old
`docs/spectral-reference.md` 2–5 ms latency claim and its CONFIRMED internal FFT
claims must not guide implementation. The screenshot document's post-gain weight
multiply is also not the current kernel's behavior.

## 1. Common control model to prototype first

Keep analysis separate from audio rendering. All renderers consume one target
reduction curve and expose their actual resulting response to the UI. Work in
double precision. Frequencies satisfy `0 < f < 0.49*fs`; handle DC separately.

### Detector and absolute calibration

Use periodic Hann analysis, initially `N=4096`, with sample-rate-scaled N where
needed to preserve seconds of observation. Quality changes hop cadence, not
just N: prototype Normal/High/Ultra with `H=256/128/64` at 44.1/48 kHz. These are
engineering starting points, not measured soothe values. Test a second, longer
low-frequency window if 80–200 Hz two-tone discrimination fails; do not claim
the smallest resolvable separation equals `fs/N` (window main-lobe width matters).

For a real FFT spectrum X and window w, use
`a[k] = 2*abs(X[k])/sum(w)` for positive interior bins; DC/Nyquist omit the 2.
Then `L[k] = 20*log10(max(a[k], 1e-12))` is peak-amplitude-referred dBFS for an
on-bin sine. Off-bin accuracy requires interpolated peak fitting or integrated
main-lobe energy; verify that rather than calibrating a bin-alignment accident.
Silence produces no new suppression; release existing reduction naturally.

Default linked candidate is a **power sum**, never signed waveform addition:
`a_link = sqrt((a_A*a_A + a_B*a_B)/2)`; normalize by actual channel count in mono.
For link fraction l use `a_det,c = sqrt((1-l)*a_c^2 + l*a_link^2)`.
This is phase-insensitive and has exact shared/dual endpoints. It is not claimed
to match the manual's unspecified summation domain. Probe identical, anti-phase,
one-sided, and uncorrelated stereo to decide RMS sum versus magnitude sum and
normalization before freezing Hard-mode fixtures. Do not silently call MAX a sum.

For M/S, encode/decode with `1/sqrt(2)` and keep output/history in L/R. Mono ignores
M/S. Apply focus to processing strength or channel-local curve, not the input
audio level (which would accidentally change Hard detection). Mono sidechain
drives both target channels; stereo sidechain is analyzed in the selected domain.

### Soft and Hard

Soft retains the independently checked local octave-median background E as a
starting approximation: `e = 20*log10((a+eps)/(E+eps)) + W_c(f)`.
The existing calibrated law `R = M(depth,detail)*0.5*(1+tanh((e-C)/K))` is a
baseline, not a mandate to preserve old constants after changing FFT/window.
Depth zero explicitly gives unity. Calibration must operate on final audio,
not just frame-bin reduction. With nonzero depth this law can slightly reduce
broadband material and ordinary musical harmonics; do not promise classification
of only unwanted resonances.

Hard uses absolute calibrated level:
`z = L + W_c(f) - T(depth)`, `R = slope * softplus_knee(z)`.
Use the compressor-style smooth positive part with width k=6 dB initially:
`softplus_knee(z)=0` for z<=-k/2; `(z+k/2)^2/(2*k)` inside the knee; `z` above k/2.
Initialize slope=0.75 and `T(depth)=-6-6*depth` dBFS for the portable prototype;
record these as our designed defaults. Calibrate against new, controlled host
measurements later; old staircase bounces are secondary diagnostic evidence,
not authoritative fixtures when the captured settings are ambiguous.
Clamp the design's absolute maximum to
40 dB. Depth must monotonically lower T; it must not merely reuse Soft with a
new name. A spectral flatness/selectivity weighting may be necessary if plain
absolute-bin Hard unduly suppresses noise; add only if measured evidence needs it.

### True per-frequency Detail and independent tilt

Use smooth shelf coordinates, for prototype:
`sL(f)=1/(1+(f/500)^4)`, `sH(f)=1/(1+(2000/f)^4)`.
At 300/500/1000 Hz the low shelf is approximately .885/.5/.059. The high shelf
is its log-frequency counterpart. These particular slopes and pivots are ours.

Six independent parameters: detailLo/detailHi, attackLo/attackHi,
releaseLo/releaseHi. Candidate per-frequency Detail:
`d(f)=clip(detail + detailLo*sL + detailHi*sH,0,10)` with tilt controls in [-5,5].
Use d(f) for **all** Detail-dependent quantities: notch width, cut sensitivity/
depth, and Soft detection smoothing. A depth-only tilt is insufficient.
Starting monotone width curve is `b(d)=0.5*2^(-d/2)` octaves, with monotone
strength multiplier `m(d)=2^((d-5)/10)`. Use the existing depth-5/detail-5
Soft ceiling only as an initial scale: `M=16.167635300821875*(depth/5)*m(d)`.
These replace the old nonmonotonic interpolation anchors. Test monotonically
narrower target smoothing and stronger target cuts; independently measure the
realized audio response. Historical nonmonotonic width measurements may be
confounded by depth/measurement conditions and must not dictate these formulas.

Use per-bin normalized frequency smoothing, with radius derived from b(d[k]),
and explicit weights summing to one; compare a prefix-sum implementation against
a direct weighted loop. Varying destination-bin width is not a convolution.
For Soft's separate detector smoothing, start with
`tau_det(f)=0.001*2^(d(f)/2)` seconds, then measure and tune.

Expose attack/release as normalized 0–10 controls rather than literal ms if
matching the product's interaction. Candidate 1 kHz maps:
`tA(u)=0.001*27^(u/5)` s and `tR(u)=0.005*10^(u/5)` s.
Use `t_base(f)=t_1k*clip((1000/f)^0.5,0.25,4)` initially for both;
each low/high tilt multiplies time by `2^(tiltLo*sL + tiltHi*sH)` with tilt [-2,2].
Apply `alpha=exp(-H/(fs*tau))` at actual frame cadence. Retain the existing
decoupled attack/release law, independently checked with its step response.
The exponents, ranges and maps require measurement; they are not soothe constants.

### Curves, band focus and parallel bandpasses

Represent slot existence, enable, and selection separately. Bypass preserves all
band settings and removes only that band's influence; deletion frees the slot.
Keep the existing shared response evaluator for EQ-style depth curves, with
shape-specific Q/slope controls. Eight types are enough; an extra decorative
shape whose audio semantics duplicate another is unnecessary.

For ordinary enabled curve bands, sum dB responses. With band focus p in [-1,1],
candidate factors are `qA=1-max(p,0)`, `qB=1-max(-p,0)`; multiply that band's
dB contribution by qA/qB. Fix UI sign labels consistently. Focus affects a
band's contribution, including negative depth, not all suppression in that area.

Bandpass constraints must combine as a **union**, not multiplying individual
bandpasses. Candidate masks `B_i=clip(abs(H_BP,i),0,1)` combine as
`B_union=1-product(1-B_i)`; none present means B_union=1. This is bounded and
passes disjoint regions; it is not asserted to be the proprietary parallel law.
Apply `R_target *= B_union * rangeMask` after sensitivity and smoothing so
out-of-range reduction is actually suppressed. The range mask uses smooth
transition skirts and unity when default boundaries are inactive. If bandpass
depth/focus participates in the summed curve, keep mask gating and depth weighting
separate. For a bounded continuous focus candidate, compute per-channel masks
as follows: `U_c=1-product(1-q_i,c*B_i)` and `qmax_c=max(q_i,c)` over enabled
bandpasses; then `mask_c=1-qmax_c+U_c`. With no enabled bandpass mask_c=1.
At uniform centered focus this is the union above; at zero contribution from
all bandpasses it becomes unity continuously. Since U_c can exceed qmax_c for
overlapping bands, clip mask_c to [0,1]. This is explicitly our routing rule,
to be replaced only by controlled evidence. Probe overlapping and disjoint
bandpasses; never silently multiply their individual masks.

### Max cut, monitoring and UI truth

Max cut is an explicit enabled flag plus value, not a magic numeric Off sentinel.
Enforce it on the final frequency-response reduction after spatial/temporal
smoothing; reducing the cap must also constrain old envelope state. Smooth the
cap itself over 20 ms and define the guarantee relative to that instantaneous
smoothed cap, so automation does not click or secretly violate its promise.
For cascaded filters, bounding each section alone does not bound their sum.

Normal output: `outGain*((1-mix)*dryAligned + mix*wetGain*wet)`.
Delta candidate: `outGain*(dryAligned-wetGain*wet)`, ignoring Mix. Delta polarity
and whether trim belongs in reference Delta require a direct probe; preserve
existing project behavior until evidence justifies changing its sign/algebra.
Band listen is an audition path: apply a band-region filter to Delta (default)
or dry audio (solo option); do not recompute a different suppression graph just
because monitoring is enabled. Sidechain listen auditions the selected external
source, aligned to output latency. Define precedence: host/UI bypass > sidechain
listen > selected-band listen > delta > normal. Crossfade monitor changes.

External SC selects the detector source for BOTH Soft and Hard; it does not
replace audio input. Missing/disconnected SC yields no new reduction, with
release of existing cuts, rather than silently analyzing main input. Keep SC
enabled and SC-listen independent. No invented third SC algorithm is required.

Draw the actual mixed response: `Hmix=(1-m)+m*wetGain*Hwet`.
`-20log10(abs(Hmix))` is required when phase varies; multiplying dB reduction by
Mix is incorrect. Publish separate channel responses in unlinked operation.

## 2. Rendering architecture: real latency and phase behavior

**Selected architecture for Stage 2:** one target curve, two FIR phase designs,
and one causal convolver with a direct head plus partitioned tail. This preserves
dense spectral shapes without peak picking or fitting overlapping IIR notches.
It is a selected prototype architecture, not yet a passed implementation.

### 2.1 Minimum-phase FIR synthesis

Use an even power-of-two design grid M, initially 8192; first test FIR lengths
L=2048 and 4096. These are capacity candidates, not Quality settings. Interpolate
the target reduction onto positive design-grid frequencies, with real DC/Nyquist
endpoints. Set `A[k]=10^(-R[k]/20)`; clamp R to the design's 40 dB maximum before
taking logs, so A is strictly positive. Mirror log magnitudes to a real even
M-point sequence. Natural logarithms are required below:

`c = real(ifft(log(A_full)))`.
Construct `cmin[0]=c[0]`, `cmin[1:M/2]=2*c[1:M/2]`,
`cmin[M/2]=c[M/2]`, and `cmin[M/2+1:M]=0`.
Then `Hmin=exp(fft(cmin))` and `hfull=real(ifft(Hmin))`.
The full design-grid magnitude matches A up to numerical precision; **this alone
does not prove that truncating hfull is accurate or strictly minimum-phase**.

Take the first L taps initially without an extra window. Measure omitted-tail
energy and dense-grid response; increase M/L if circular wrap or tail truncation
fails. A terminal taper is a separately measured candidate, not an invisible fix.
Do not normalize DC or energy afterward: doing so changes the requested curve.
Use exactly `[1,0,...]` for the unity target. Compare against scipy's homomorphic
[minimum-phase conversion](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.minimum_phase.html)
on symmetric reference FIRs with `half=False` and a large explicitly fixed n_fft.
Also check independently that real/imaginary parts of log H satisfy the periodic
Hilbert relation on the untruncated design grid. For small constructed examples,
check zeros inside the unit circle; roots of a 4096-tap polynomial are not a
well-conditioned acceptance test. Finite-grid/truncation approximation must be
reported, rather than asserting a mathematical guarantee the algorithm lacks.

### 2.2 Exact zero-buffer-delay convolution indexing

Let P=64 samples initially; L is zero-padded to a multiple of P and J=L/P.
Partition `h_j[r]=h[j*P+r]`, `0<=r<P`, and store the 2P-point FFT
`F_j=FFT([h_j,zeros(P)])`. Partition the input the same way as x_b, with negative
block indices identically zero, and `X_b=FFT([x_b,zeros(P)])`.

For sample n in block b, the direct head is
`head[n]=sum(r=0..P-1, h_0[r]*x[n-r])`.
For the tail, define `Z_b=sum(j=1..J-1, F_j*X_(b-j))` and `v_b=real(IFFT(Z_b))`.
The completed tail block is `tail_b=v_b[0:P]+v_(b-1)[P:2P]`.
At the beginning of block b every X needed for Z_b is already known. Thus the
tail is ready before its first output sample, while the head uses current input:
`y[n]=head[n]+tail_b[n-b*P]`. No P-sample output FIFO delay is introduced.
Use no STFT windows or COLA normalization in this convolver; these are ordinary
zero-padded linear-convolution blocks. Detector framing is independent.

Store at least J+1 historical input spectra per input channel and a P-sample
direct-history ring. Process arbitrary host block lengths with an internal P
counter; never align partitions to host callbacks. Verify this exact indexing
against `scipy.signal.fftconvolve` and `lfilter`, including nonzero taps precisely
at P-1, P, 2P-1, 2P and L-1, impulses at every partition offset, and final flush.

### 2.3 IR transitions without cold starts

Two preallocated coefficient banks share the complete input history. Admit a
new IR only on an internal P boundary. Both banks must evaluate the **same past
input**, including samples before the new IR was produced. On a bank's activation
at b, compute BOTH its Z_b and Z_(b-1) using that bank's coefficients; carrying
the old bank's overlap term into the new bank is incorrect.

Crossfade outputs with `t(q)=0.5-0.5*cos(pi*q/F)`, q=0..F, and
`y=(1-t)*y_old+t*y_new`. Use F=H initially, where H is the Quality control hop
and a multiple of P. Retarget only after the previous fade ends; if several
requests arrive meanwhile, retain the latest pending target in preallocated
storage. No bank deletion/free, allocation, FFT planning, locks or I/O occurs
in processing. A synchronous, bounded synthesis first establishes deterministic
tests; moving synthesis to a worker later must not silently make missed deadlines
or nondeterministic control timing the product's normal behavior.

This transition is exactly equivalent to the time-varying kernel
`h[n,r]=(1-t[n])*h_old[r]+t[n]*h_new[r]`, which supplies an independent direct
sum oracle. Raised-cosine weights avoid a coefficient step but do not eliminate
all modulation artifacts. Two minimum-phase endpoints do not make every blend
minimum-phase; phase cancellation can also make a blend deeper than either cut.
Measure the entire transition, including actual `abs((1-t)*Hold+t*Hnew)` on a
dense grid and intermediate t values. A Max-cut promise cannot rely only on
clipping R at endpoint synthesis. If its tolerance fails, synthesize intermediate
curves or otherwise revise the transition; do not relax the promised bound.

### 2.4 Linear phase, lookahead and actual reported latency

For a linear-phase candidate use an odd tap count `Llin=2D+1`; initialize
D=2048. Design a symmetric FIR from A with `scipy.signal.firwin2` as the independent
reference, and compare a frequency-sampling/IFFT construction numerically before
porting. Both kernels in a transition share D, so their weighted sum remains
symmetric. A frozen kernel has group delay D; a time-varying system should not
be advertised as one fixed LTI transfer function. Increase Llin if the 54 Hz
notch or max-cut/attenuation-only tolerances fail.

Normal/High/Ultra use H=256/128/64 initially with one common design capacity;
both detector updates and admitted IRs actually follow that cadence. Normal
minimum-phase adds an explicit audio delay B for lookahead, initially B=N/2
(2048 with the proposed detector). Analyze undelayed main/SC audio and convolve
the delayed main audio. Low-latency sets B=0 and forces Normal/minimum-phase.
Linear phase can use that same explicit lookahead B and reports `B+D`; minimum
phase reports B. The direct-head convolver contributes **zero additional delay**.
Dry/monitor paths are delayed by the corresponding B or B+D. Validate all
numbers by impulse/null tests rather than copying soothe's table.

The minimum-phase kernel's frequency-dependent group delay is not an extra
integer host latency. Likewise a trailing detector window causes late reaction
without requiring delayed output. At high rates keep duration and capacity
tradeoffs explicit; a resampling-free low-latency branch can still report zero.
Changing phase, capacity or explicit delay must rebuild the appropriate state
off-thread and notify the host; transport-safe mode changes and history warming
need an integration test, not just a parameter dropdown.

The CPU gate is essential: measure two active convolution banks per stereo
channel, a new IR at every Ultra hop, and high-rate operation. Pick M/L/P from
response and worst-case timing measurements before freezing the port spec.
If a fixed bank of IIR cuts is revisited for performance, treat it as a separate
approximation requiring new dense-spectrum checks; it is not the chosen design.

The [ShortTimeFFT reference](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.ShortTimeFFT.html)
remains an independent check for analysis framing or any retained WOLA reference;
it does not define the new partitioned convolver.

## 3. Prototype gates and stage handoff

**First bounded portable slice:** implement the exact direct-head/partitioned-tail
FIR renderer and its transition oracle, then connect Soft/Hard, monotone Detail,
six low/high tilts, shared/dual detection, per-channel/band focus and parallel
range masks. Implement control math with explicit Python inputs; do not change
the plugin's public parameter schema until the reference host maps real parameter
ids/ranges and the root decides migration. This slice does not depend on presets,
band-listen UI, sidechain bus UI, or exact sonic calibration. Each can consume the
same verified renderer/control functions afterward. Track acceptance and product
integration in `docs/soothe3-parity-progress.md`.

**Numerical acceptance before first port:** float64 convolution versus scipy
and direct time-varying sum has maximum absolute error <1e-10 for normalized
fixtures; arbitrary host partitions differ by <1e-10. Cepstral full-grid
magnitude relative error <1e-10. Realized truncated-FIR magnitude error <=0.25 dB
for the stated narrow/broad target suite, including dense interpolation between
design bins. Frozen/transition attenuation-only ripple <=0.01 dB and cap
overshoot <=0.05 dB are explicit initial engineering tolerances, not exact
mathematical claims. If these fail, adjust design capacity/transition method,
not test limits. Print all maxima, tail-energy fractions and resulting capacities.
Control-map endpoint, monotonicity, symmetry, and mask identities use 1e-12
absolute tolerance. Mode latency/null errors use the convolution tolerance.
Benchmark actual C++ later; Python wall time cannot certify an audio deadline.

Sonic calibration is a separate, visible result: these gates prove our specified
DSP, not soothe equivalence. Controlled comparison mismatches must stay in the
ledger rather than being concealed by fitting undocumented old bounces.

1. Pure control math: direct-loop smoothing versus optimized form; exact focus,
   link and bandpass union endpoints; monotonic depth; six independent tilts;
   DC/Nyquist/silence; cap automation after a deeply attenuated history.
2. DSP identities: independent scipy checks of each filter at 44.1/48/96/192 kHz,
   phase/response/truncation, float64 impulse and random-block partition agreement.
   Depth zero, bypass, Mix zero and default trim must null against aligned dry.
3. Renderer selection: narrow 3 kHz tone, 80/100 Hz pair, dense harmonic chord,
   broadband noise, and a moving resonant tone. Verify actual notch widths,
   off-target attenuation, mixed response, and 40 dB/Max-cut limits. Reject or
   revise FIR capacity/transitions if these fail; do not hide gaps in UI labels.
4. Use existing bounces in `proto/out/soothe3_kit/out/` for same-input attenuation,
   FWHM and timing. Existing measured detail peaks are 2.9/8.2/13 dB and measured
   widths 221/323/54 Hz for detail 0/5/10 at the 3 kHz probe. These are historical
   observations, not universal knob formulas. Repeat new host captures with full
   parameter and latency metadata; investigate disagreement before recalibrating.
5. Report independently: Soft level-invariance spread over 36 dB, Hard level-sweep
   transfer, tone suppression, nonresonant-bed attenuation, modulation sidebands,
   attack/release versus frequency, transient pre/post energy and stereo image.
   Use direct spectral amplitude fits, not only Welch plots or the UI ribbon.
6. Render level-matched WAV triplets (dry/reference/ours) from owned vocal, drum,
   bass and mix material if present; otherwise identify synthetic fixtures as
   synthetic. Automated metrics are not a listening verdict. Leave unclipped raw
   measurement WAVs distinct from normalized audition files.
7. Export fixtures only after formulas and selected architecture pass. The port
   must include allocated memory bounds, bounded-work counts, and explicit
   allocation/delete/locks/IO/logging audit, plus build/console/pluginval gates.

Reference captures must first verify plugin identity, parameter readback, no
demo interruptions, transport/sample rate, sidechain routing and host latency.
Never loosen acceptance criteria to relabel a failed feature as complete.
If low-latency or linear-phase fails its renderer gate, keep the work pending;
an inert mode selector is not an implementation.
