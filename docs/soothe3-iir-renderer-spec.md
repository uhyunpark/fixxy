# Bounded minimum-phase IIR renderer candidate

2026-10-18. Stage 1 handoff; **not accepted or production approved**; our approximation, not a sonic-match claim.
Scope: macOS AU/VST3/Standalone, eventually 16 channels; v1–v3 sessions retain
legacy WOLA. New renderer/control semantics require a new state/engine version.

## Decision and traps first

Prototype **one cascade of finite-depth matched cut bells**, K=16 first and
K=24 absolute ceiling, with fixed slot identities and double DF1 histories.
Reuse `MatchedCoeffs.h::computeBiquad(Bell,...)` and its exact response evaluator;
do not instantiate 24 dynamic-EQ detectors or copy `MultibandEq` output fades.
Reserve two of K slots for finite low/high shelves, initially disabled.
Render cost is small enough to investigate; fitting/control cost is unproven.
Start with static target fitting. If K24 cannot fit required targets, reject
this architecture before implementing asynchronous scheduling or a phase UI.

True Notch has unit-circle zeros/infinite cut: it fails a finite Max-cut promise
and strict minimum phase. Finite bells avoid that structural problem. Frozen
stable poles do not prove stable modulation. DF1 preserves literal sample
histories but does not remove modulation artifacts. Endpoint-safe filter banks
can cancel when output-crossfaded; do not reuse the rejected two-bank argument.
Per-section limits do not bound the cascade by that same limit.

Fresh evidence is `soothe3-renderer-probe-2026-10-17.md`, read in full:
48 kHz/H64 vDSP automation now fits sampled hop deadlines (1150.983 us mean,
1221.833 us max); 96/192 kHz still fail relevant rows. H512/1024 are unsupported,
not tested slow schedules. Removing coefficient setup alone does not repair
the measured 192 kHz admission floor. This replaces the old 2230 us comparison.

## 1. Inputs, section maps and independent references

Input is a finite target reduction R(f)>=0 in dB, already containing detector,
time/frequency smoothing, focused curves and parallel masks; target cap C<=40.
Keep this successor contract separate from legacy tilted Soft's >40 dB demands.
The renderer must not reinterpret band weights as filters on the audio itself.
Interpolate target in log frequency between positive source bins; handle DC
and Nyquist separately. Log interpolation cannot recover unresolved detector
peaks: fitting analytic targets and fitting actual FFT targets are separate gates.

Each active bell has f in [20,min(20000,0.45*fs)] Hz, Q in [0.25,128], and
cut d in [0,40] dB; call the shared recipe with gainDb=-d. These are candidate
engineering bounds. Gain zero is an explicit exact identity, not a near-cancelled
recursive section. Shelves use Q=1/sqrt(2), negative gain only; reject any
matched shelf with an unaccepted boost/undershoot rather than assuming monotonicity.
No low/high-pass or band-reject audio sections are needed for suppression masks.
Optional flat attenuation b=min R over the **whole** target grid is factored as
10^(-b/20); it cannot be inferred from only the region where suppression is wanted.

For a residual peak, find half-height frequencies fL,fH and seed
BW=log2(fH/fL), Q0=1/[2*sinh(ln(2)*BW/2)], clamped to bounds.
This is the analog midpoint-dB bandwidth convention, only an initial guess for
the matched digital response. Measure final width, particularly near Nyquist.
The [W3C Audio EQ Cookbook](https://www.w3.org/TR/audio-eq-cookbook/) supplies
independent RBJ bell/SOS and bandwidth conventions; it is not an oracle asserting
that the repository's matched filter must equal an RBJ filter at high frequencies.
Use existing verified `proto/matched_kernel.py` coefficients, independently
evaluate with SciPy `freqz_sos`/`sosfilt`, and compare low-frequency analog limits.

For every candidate, form roots of z²+a1*z+a2 and b0*z²+b1*z+b2.
Require all finite, b0>0, every pole/zero modulus <1 (record minimum margin).
Also check real second-order Schur inequalities 1±a1+a2>0 and 1-a2>0;
apply the same test to normalized numerator b1/b0,b2/b0. Do not impose a
large arbitrary radius margin that silently removes narrow low-frequency cuts.
Reuse coefficient formulas verbatim; the established sigma and frequency
placement correction are load-bearing. `BandResponse.h`'s analog curve alone
is not the drawn response of the matched bank.

## 2. Deterministic bounded fitting, no general optimizer in the callback

Build at most 705 fitting samples: 513 logarithmic frequencies including the
audible endpoints, DC/Nyquist, and up to 64 strongest source-grid local peaks
with their two half-height crossings. Duplicate frequencies are merged. Detect
peaks in one bounded source-grid scan (capacity 8193 bins); retain best 64 by
fixed-size insertion, ties resolved by lower frequency. Plateau uses its midpoint.
This avoids missing the 54 Hz-wide 3 kHz target on a coarse log-only grid.
Reference tests use a much denser independent grid; fitting samples are not proof.

Set current fitted reduction A=b. If nonzero DC/Nyquist residual requires it,
fit corresponding shelf first: center is its first inward half-height crossing,
Q fixed above; otherwise leave that reserved shelf at identity. Then repeat at
most K-2 times: choose largest positive R-A, lower-frequency tie; stop if <=0.05 dB.
At that peak, use residual half-height crossings (extend to the fitting boundary
if absent), and try Q in clamp({Q0/sqrt(2),Q0,Q0*sqrt(2)}). For each Q, solve
center cut by exactly 8 bisections on d in [0,40], using the actual digital
center response. First verify center attenuation is monotone in d over these
bounds; otherwise reject the recipe/range, do not silently bisect it.
Score each candidate on 9 local probes: center; log offsets ±BW/4, ±BW/2,
±BW, ±2*BW, clipped to supported band. Score = sum(error² + 3*positive
overcut²), where error=A+candidate-R; ties prefer smaller Q then smaller d.
Evaluate only the winning candidate over the full fitting grid and add it to A.
Retain **signed** residual for scoring; only peak selection clips negative residual.
No unbounded solve, retry-until-convergence, or iterative section-count expansion.

After fitting, run the cascade cap test below. For a failed proposal, try common
depth scales {0.875,0.75,0.625,0.5,0.375,0.25,0.125,0}; first passing scale wins.
Recompute actual responses, not scaled dB guesses. These 8 bounded retries are
a safety fallback, not a fit-quality waiver. Report error after scaling. A zero
bank is safe but a failed fit whenever the target is nonzero. Do not add boosts
to repair bell tails outside masks. Record target peaks dropped at the K ceiling.

## 3. Whole-cascade cap and attenuation tests

H(f)=10^(-b/20)*product Hj(f); A(f)=-20*log10(abs(H(f))). Sum actual section
responses, not requested depths. Static acceptance requires no significant
above-unity response and A<=C, including overlap between multiple nearby cuts.
Checking each dj<=C is insufficient; sum of section maxima<=C is a valid but
often excessively restrictive sufficient bound, useful as an emergency fallback.

A bounded frequency-interval certificate is available without a high-order
polynomial root solver. For c=cos(w), a real quadratic numerator's squared
magnitude is N(c)=(b0-b2)²+b1²+2*b1*(b0+b2)*c+4*b0*b2*c²;
D(c)=(1-a2)²+a1²+2*a1*(1+a2)*c+4*a2*c². Over each c interval, find exact
quadratic extrema from its endpoints and contained vertex. With Dmin>0,
Nmin/Dmax <= |Hj|² <= Nmax/Dmin. Multiply section lower/upper bounds and
flat gain²; compare to 10^(-C/10) and 1. Use logs only if products risk underflow.
Start with 128 equal-angle intervals; bisect uncertain intervals, longest first,
up to 4096 leaves and depth 24. Failure to certify is **unresolved**, not success.
Round interval bounds outward; an engineering epsilon without outward error
accounting is numerical evidence, not a mathematical continuum certificate.
Cancellation in polynomial coefficients near DC must be checked against extended
precision; clamp neither a negative lower bound nor Dmin to fake a certificate.
For the first Python experiment also densely sample >=65537 linear frequencies,
add adaptive local extrema around each center, and cross-check both calculations.

Apply this to every actual admitted coefficient snapshot during automation.
While coefficients are constant within a microblock the frozen response is
bounded; historical recursive state is still present. This is **not** a universal
sample-by-sample attenuation lower bound for a time-varying signal. The same
distinction applies to any user-facing effective cap and reduction display.
Cap cost and failed-certification rate are mandatory outputs, not hidden overhead.

## 4. State continuity and coefficient motion

Associate new bells with current slots greedily by increasing absolute log2
frequency distance, then Q distance, then slot index; only match within 1/3 octave.
Matched slots retain DF1 x1,x2,y1,y2 throughout. Do not reorder cascade sections
to frequency order: that changes each section's input/state history.
Unmatched old slots first ramp depth to zero; unmatched new proposals wait for
an identity slot. Latest target replaces a pending proposal; no growing queue.
For a zero-depth target, snap d<=1e-6 dB to zero; then output literal dry and set
y1=x1,y2=x2 (same physical section input history), then retune while at identity
and ramp depth in. Never reuse a deep old y history after changing frequency.
This is a new activation policy to test, not `MultibandEq`'s cold-start fade.

First dynamic prototype: fit targets at a 100 Hz clock; microblocks
U=max(1,round(fs/1500)) samples, phase independent of host block boundaries.
At U boundaries one-pole smooth log(f), log(Q), d with tau=10 ms, then compute
new matched coefficients outside the sample loop. Hold coefficients inside U.
This clock/tau is our test candidate, not a proprietary quality setting. Fit
cadence, detector cadence and coefficient cadence must be reported separately.
For each proposed microstep test poles/zeros and full bank cap. On failure,
bounded depth backoff above applies; use exactly those checked coefficients,
without another unchecked interpolation, and publish their effective cap. Backoff
can introduce a discontinuity and must pass the artifact gate. A falling cap that cannot be admitted is a
failed responsiveness test, not permission to silently hold an old deeper cut.
Prototype may compute all checks offline first; C++ must subsequently meet their
full throughput budget. Do not call a checked offline trajectory realtime-ready.

Use DF1 recursion from `SvfBandNode.h:289`, double intermediates between sections;
only cast to float at the plugin boundary. Dynamic tests must independently
implement the varying-coefficient scalar recurrence; static SciPy SOS state
is not interchangeable with DF1 state at a coefficient update.
[Wishnick, DAFx 2014](https://www.dafx14.fau.de/papers/dafx14_aaron_wishnick_time_varying_filters_for_.pdf)
explains why time-varying stability and state realization require separate care.
Existing dynamic-EQ gain tests do not cover this bank's simultaneous f/Q motion.
If DF1 movement fails, reject that transition policy; evaluate the existing TPT
bell realization in a separate prototype before replacing its high-frequency
response. Do not revive the ill-conditioned normal-form state transformation.

## 5. Delay, detector resolution and separate linear phase

The IIR recurrence has direct feedthrough: unity impulse starts at sample zero,
and no input/output block FIFO is required. This permits zero **added buffer**
delay in low latency; frequency-dependent phase/group delay and detector age
still exist. Do not retain WOLA rendering or merely report zero latency.
Keep detector N/H unchanged in the first comparison to isolate renderer effects.
N4096/48 kHz has an approximately 42.7 ms window-center age; changing rendering
does not recover a poorly resolved 80/100 Hz pair or remove that detection lag.

Default lookahead candidate delays audio/dry/listen by D=round(N/2)+H samples,
with undelayed detector input. This roughly aligns window-center evidence plus
one analysis hop to audio; it is our chosen budget, not original latency counts
or a complete detector-delay compensation. Measure burst/vibrato timing and
target age, including fitting/scheduling. Low latency D=0 sacrifices lookahead.
Report actual D; no callback waiting for a worker. If asynchronous fitting is
later chosen, bound observed target age and stale-target policy separately.

Linear phase is a **different renderer**: sample the accepted bank magnitude
A[k]=abs(Hbank[k]) on an even M grid; one real inverse FFT gives periodic even
h. For odd L=2Dfir+1, take h[-Dfir..Dfir] around zero, apply an explicitly chosen
symmetric window, and place it causally at indices 0..L-1. Its symmetry gives
constant group delay Dfir; added lookahead adds to reported latency. Try
L=2049 then4097 at 48 kHz with rate-scaled lengths, M>=4*(L-1). No feasibility
claim: truncation/windowing changes magnitude, cap and ripple, requiring full
independent gates. A real impulse can have negative lobes; do not clamp them.
Use ordinary partitioned convolution with measured partition buffering. Its
changing FIR needs its own history/cap-safe transition test, even if symmetric
endpoints share delay. It is outside the first IIR prototype.

## 6. Acceptance order, numerical budgets and rejection decisions

Stage 2A: static K16/K24 fit tables first. Include 54 Hz FWHM notch at 3 kHz;
80/100 Hz pair at several widths; 8/16/32/64 resonant harmonics; broad buildups;
disjoint masks/protected gaps; overlapping/edge cuts up to40 dB; DC/Nyquist;
44.1/48/88.2/96/176.4/192 kHz. Include targets generated from known matched
banks as an exactly representable control and independent Gaussian/log-targets.
Measure maximum/RMS dB error, overcut outside masks, peak position, midpoint
width, pole/zero margins and cap excess. Do not call the previous 0.25 dB
FIR target tolerance achieved: test it unchanged and report failures explicitly.
Also report diagnostic counts within 0.5/1/2 dB, but these are not automatic
acceptance thresholds. If K24 misses the required error budget, root must choose
an explicitly approximate product target or reject this bank; do not increase K
without a renewed CPU budget or silently weaken assertions.

Stage 2B only after static acceptance: SciPy static SOS impulse/noise agreement
<=1e-10 double; varying DF1 oracle <=1e-10; exact unity and arbitrary chunks;
moving tone/chirp/vibrato, moving/crossing peaks, frequency jumps, Q128 sweeps,
0↔40 dB cuts, rapid cap falls, silent input with retained state, 60-second
adversarial parameter sequences, identity slot recycling and long tails.
Log measured modulation sidebands, peak overshoot, spectral error and settling
versus a per-sample-smoothed reference and render WAVs. Proposed screening gates:
no nonfinite/growing zero-input tail, <=0.5 dB extra peak versus that reference,
automation-difference RMS <=-60 dBFS for -12 dBFS test signals. These are new
budgets requiring explicit report, not previously passed results or universal
audibility thresholds. Raw response cap excess <=0.05 dB and above-unity ripple
<=0.01 dB are numerical gates; certificate must state its own outward-error bound.

## 7. Feasibility arithmetic and final gate

K24*16*192000=73.728 million section-samples/s: DF1 needs 368.64M multiplies
and294.912M adds/s, plus routing/smoothing. K16 needs two thirds. This is
plausible arithmetic, not a timing result; dependency chains/vectorization matter.
Four double histories per section/channel consume only12 KiB at K24/16 channels.
One greedy fit uses <=K*(705+3*8+3*9) response evaluations, excluding shelves
and cap retries. At100 fits/s*16 independent lanes that is up to29.03M response
evaluations/s before cap work. Transcendentals and interval checks can dominate;
up to4096*K interval evaluations per certification is a serious additional cost.
No assumption that moving those operations to a worker creates throughput.

Final C++ gate must include detector, fitting, every cap/transition check, SC,
all curves,16 unlinked channels,32/64/128/256/1024 and odd callbacks, every rate,
automation and multiple instances. Record phase costs, p99/max, allocation/
delete/lock/I/O/logging audit, target age and misses. Proposed product budget:
mean <=25% and sampled p99 <=50% of callback duration on the named M1; any
deadline miss is a recorded failure requiring investigation. Do not drop gates
to approve a nice static curve. If control work fails, simplify/replace that
bounded fitter/certificate with independently verified math, or reject the
candidate; retain legacy WOLA and leave phase/quality controls unavailable.
