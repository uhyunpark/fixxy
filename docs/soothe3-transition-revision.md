# Phase-limited FIR transition: Stage 1 revision

Date: 2026-09-11. Research handoff only; not a passed prototype or authorization
to port. A separate Stage 2 agent must implement and independently verify this
feature, and a different Stage 3 agent must port it. This remains our designed
DSP approximation, with no soothe algorithm or sonic-equivalence claim.

## Decision

The proposed cosine lower bound is correct under explicit **actual endpoint**
magnitude and phase assumptions. Retargeting once per actual quality hop avoids
the rejected queued five-segment scheme. Approve this revised candidate for
Stage 2 experiments, not production: finite-tap/inter-bin phase bounds, moving
targets, throughput, and the new effective-cap semantics still need their gates.

The original claim against an independently moving 20 ms cap is rejected even
for pure scalar gain. The revision distinguishes the requested cap, its 20 ms
control smoother, and the effective cap admitted by the audio transition. Only
the last is the instantaneous cap guarantee and must be published to the UI.
It may settle later than 20 ms. Do not silently label requested-cap compliance
as achieved while the effective value remains higher.

## State, conventions, and candidate update

Use the verified static capacity M=16384, L=4096, P=64 at 44.1/48 kHz. Other
sample-rate capacities need their existing independent synthesis gates and CPU
measurements. Quality hops are H=256/128/64, all multiples of P. One update
consumes the newest target at each hop; there is no subdivision queue.

Let `ell_current[k]` be the **stored causal-cepstrum log spectrum of the last
fully admitted endpoint**, before exponential/IFFT/truncation. It is not
`log(FFT(truncated_taps))`. Compute `ell_target` from the latest capped,
smoothed target with the verified real-cepstral recipe. This construction uses
natural logarithms and the existing NumPy-compatible FFT normalization.
The relation between a causal cepstrum and minimum phase is established in
[Julius O. Smith, Minimum-Phase and Causal Cepstra](https://www.dsprelated.com/freebooks/sasp/Minimum_Phase_Causal_Cepstra.html).

Define, on the design bins:

```
delta[k] = ell_target[k] - ell_current[k]
Dgrid = max_k abs(imag(delta[k]))
phi_limit = 2*acos(10^(-0.04/20))
          = 0.19179375027524279 rad = 10.988972427757481 degrees
alpha = 1                           if Dgrid == 0
        min(1, phi_limit / Dgrid)    otherwise
ell_next = ell_current + alpha*delta
h_next = real(IFFT(exp(ell_next)))[0:L]
```

This is the uncorrected grid proposal to test first, not proof of the realized
phase bound. No wrapping or principal complex logarithm is applied to delta.
Its imaginary values already represent the consistent phase branch produced
by the causal cepstrum; subtracting wrapped `angle()` values is incorrect near
the +/-pi cut. At DC and Nyquist, enforce zero imaginary log-spectrum values
as required by the real transform. Reflect positive bins by Hermitian symmetry;
do not replace the retained Nyquist cepstrum coefficient with zero.

Keep the current state until its fade completes. At each hop boundary, admit
one new endpoint, reconstruct its convolution history with its own coefficients,
and fade over exactly H samples. For q=0..H-1:

```
t(q) = 0.5 - 0.5*cos(pi*q/H)
y[n] = (1-t(q))*y_current[n] + t(q)*y_next[n]
```

At the next boundary, the completed endpoint is current and the next fade's
q=0 output equals it. The missing explicit q=H sample is that boundary sample;
do not use H-1 in the denominator or introduce a one-sample endpoint jump.
Both banks must use identical input history. A newly activated bank must
evaluate both its current tail block and its preceding overlap block with
**its own** coefficients. Old-bank overlap is not transferable. The static
primitive does not yet supply this bank-activation operation.

## Endpoint bounds and the cosine proof

For any one frequency, write actual finite-FIR endpoint values as
`A=a*exp(i theta0)` and `B=b*exp(i theta1)`. Assume `a,b>0` and the shortest
angular separation obeys `abs(theta1-theta0) <= phi_limit < pi`.
Rotating their angular bisector to zero shows that the real part of the blend
is at least `[(1-t)*a+t*b]*cos(phi_limit/2)`. Consequently:

```
abs((1-t)*A+t*B) >= [(1-t)*a+t*b]*cos(phi_limit/2)
```

This argument is valid for every t in [0,1], unequal endpoint magnitudes
included. The upper bound is the triangle inequality:
`abs((1-t)*A+t*B) <= (1-t)*a+t*b`. Thus endpoint ripple <=0.01 dB suffices for
the same blend ripple bound; an exactly attenuation-only endpoint pair has an
exactly attenuation-only blend.

For a constant cap C, actual endpoint floors
`a,b >= 10^(-(C+0.01)/20)` combine with the chosen cosine factor to give at most
0.05 dB total cap loss: 0.01 dB endpoint allowance plus 0.04 dB transition loss.
Do not substitute the looser 0.25 dB response-approximation gate for the 0.01 dB
endpoint-cap gate. Stage 2 reported 0.00115059181 dB worst endpoint overshoot
relative to each target's sampled maximum in its M16384 static suite, but every newly generated
intermediate and moving-target endpoint needs this separate test too.

## Falling cap: explicit effective-state revision

Let `C_request` be the latest requested cap after its existing 20 ms smoother,
`C_current` the completed endpoint's effective cap, and the latest target
reduction satisfy `R_target[k] <= C_request`. The stored current ideal reduction
satisfies `R_current[k] <= C_current`. Linearity of the cepstral transform gives:

```
C_next = (1-alpha)*C_current + alpha*C_request
R_next[k] = (1-alpha)*R_current[k] + alpha*R_target[k] <= C_next
C_effective(t) = (1-t)*C_current + t*C_next
```

With actual endpoint floors `a >= 10^(-(C_current+0.01)/20)` and
`b >= 10^(-(C_next+0.01)/20)`, the weighted arithmetic/geometric-mean inequality
gives `[(1-t)*a+t*b] >= 10^(-(C_effective(t)+0.01)/20)`. The same cosine proof
therefore guarantees reduction <=`C_effective(t)+0.05` dB. This works for rising
and falling cap requests. A phase-free pure gain change has alpha=1, and its
geometric effective-cap curve remains conservative relative to the actual
linear-amplitude blend. The 0.01 dB allowance is added once, not at each hop.

The publication contract needs all three values when useful: knob/requested
target, optionally the smoothed control target, and actual effective cap. The
audio guarantee and visualization must use the effective cap at the same
sample/fade position. Control-state clamping alone does not establish it.

Why the earlier claim fails: for fs=48000, a 40->6 dB cap request smoothed with
tau=20 ms, define `C(q)=6+34*exp(-q/960)`. Fade the exact scalar gains
`10^(-40/20)` and `10^(-C(H)/20)` with the prescribed raised cosine. Independent
scalar arithmetic yields excess reduction relative to C(q) of 0.6508654586 dB
at H=256/q=39, 0.3904173546 dB at H=128/q=23, and 0.2152790070 dB at H=64/q=13.
All phases are zero and all endpoints are exact. Stage 2 must reproduce this
rejection and show the effective-cap revision fixes it, without changing 0.05.

## Finite taps, frequencies between bins, and minimum phase

The grid alpha only bounds `imag(ell_next-ell_current)` on M design bins. It
does not establish the actual truncated-FIR phase separation between bins.
Neither a small omitted **energy** fraction nor magnitude error alone bounds
phase near a deep notch. Minimum-phase conversion through finite FFTs is an
approximation; the reference explains time aliasing and FFT-size dependence in
[Smith, Minimum-Phase Filters](https://www.dsprelated.com/freebooks/filters/Minimum_Phase_Filters.html).

Measure actual `arg(B*conj(A))`, actual endpoint magnitude, and the actual
blended transfer on a dense grid, including all design-bin midpoints and
locally refined extrema. If the grid proposal fails, report the failing curve
and reject it before export. Do not loop through successively resynthesized
subdivisions while claiming one synthesis per hop. An eventual conservative
alpha could reserve a *verified* phase-error margin: if endpoint phase errors
relative to exp(ell) are bounded by delta0 and delta1 everywhere, and the
continuous ideal delta is bounded by D, require
`alpha*D + delta0 + delta1 <= phi_limit`. Those margins are not supplied by the
present static gate and must not be guessed.

For strict all-frequency certification of a measured FIR, a conservative
off-line option is the derivative bound `B1=sum_n n*abs(h[n])`. On a frequency
grid with angular spacing dw, every response differs from its nearest sampled
response by at most `B1*dw/2`. Apply this to both endpoints: subtract the larger
response radius from the minimum sampled chord magnitude, and add it to the
maximum sampled endpoint magnitude. For cap-normalized magnitudes, multiply
that radius by `max(10^(C_current/20),10^(C_next/20))` first. Adaptive/local
Taylor bounds may tighten an inconclusive certificate, but a plain dense FFT
is numerical evidence, not a universal continuum proof. Report which is used.

There is a useful stronger result for the **actual finite FIRs**. If the old
filter is strictly minimum phase, its first coefficient and the new first
coefficient are positive, and the phase-separation bound holds over the entire
unit circle, every convex coefficient blend is also strictly minimum phase.
The positive first coefficient prevents degree loss/zeros escaping through
infinity; the nonzero blend response prevents any zero crossing the unit circle.
Continuity of polynomial roots therefore preserves the old count of zeros
inside the circle. This is our homotopy deduction, not a property of arbitrary
minimum-phase endpoint pairs. The root/first-coefficient conventions are in
[Smith's minimum-phase definition](https://www.dsprelated.com/freebooks/filters/Minimum_Phase_Filters.html).
Small-order independent root checks should verify the deduction numerically;
4096th-order polynomial roots are not the primary acceptance test. Moreover,
an evolving FIR is a time-varying system: “minimum phase” describes each frozen
kernel here, not one fixed LTI response for the whole evolving output.

## Exact blend extremum oracle

Dense sampling of t can miss a narrow cancellation. Use actual complex endpoint
values and minimize analytically over all blend weights at each frequency.
Let `D=B-A`, `a=abs(D)^2`, `b=2*real(conj(A)*D)`, `c=abs(A)^2`.
Then `Q(t)=a*t^2+b*t+c` is squared blend magnitude. For a constant cap the only
interior candidate is `t=-b/(2*a)` if a>0; also test t=0 and t=1.

For the linear effective cap, minimize the **cap-normalized** squared magnitude:

```
S(t) = Q(t) * 10^((C_current+t*(C_next-C_current))/10)
lambda = (ln(10)/10)*(C_next-C_current)
S'(t) has the sign of:
lambda*a*t^2 + (2*a+lambda*b)*t + (b+lambda*c)
```

Evaluate S at both endpoints and every real root of this quadratic in [0,1],
handling linear/constant degeneracies. Maximum cap overshoot is
`max(0, -10*log10(min S))`. Require <=0.05 dB. This covers every raised-cosine
weight, not only integer audio samples. Verify the extremum implementation
against an independent scalar minimizer and dense t sweeps in Stage 2.
Above-unity ripple needs only endpoint maxima by convexity.

## Moving-target behavior, cadence, and CPU

For a fixed target, the grid phase distance decreases by phi_limit per hop
until the final alpha=1 step. With initial grid distance D0, the ideal state
reaches its target in `ceil(D0/phi_limit)` hops (one hop when D0=0). This is a
bounded phase slew, not a promise to realize an arbitrary new spectrum in
1 ms. At a constant requested cap the effective cap approaches it on that
same admitted path. A fresh moving target changes direction every hop; it must
replace the previous target immediately, without finishing an obsolete path.
Record phase-distance/error traces, target age, and settling time. A rapidly
alternating or rotating target can remain slew-limited indefinitely; do not
describe that as tracking the target exactly.

The 1 ms quantity in the candidate controls is a detector/envelope time
constant, not end-to-end response latency or a measured soothe Attack label.
The additional rendering slew must remain visible in measurements and tuning.

Computing a *new* target still needs inverse FFT(log magnitude), FFT(causal
cepstrum), and inverse FFT(exp(admitted log spectrum): **three** M transforms
per lane/hop. Retaining the old log spectrum avoids rebuilding the old endpoint;
it does not make the whole update one FFT. One synthesized new IR per lane is
allowed, plus its partition-spectrum preparation. Measure all of that, detector
work, new-bank history initialization, and four active render lanes together.
Report mean/median/p99/max and the sample-rate-specific H/fs deadline. A worker
does not repair average synthesis throughput slower than the target cadence.

## Mandatory Stage 2 gates before any fixture export

1. Check the cosine/AM-GM deductions against direct complex arithmetic for
   unequal magnitudes, all t, caps 0/6/12/20/40 dB, and positive/negative phase
   extremes. Cross-check the cap-normalized quadratic oracle independently.
2. Reproduce the scalar falling-cap failure above, then pass <=0.05 dB against
   the explicitly published effective-cap trajectory, including repeated cap
   retargets while old reductions are deep. Verify requested/effective values
   are distinct during a falling request and eventually agree for fixed input.
3. Exercise the original opposing 40 dB shelves, unity/deep cuts, the 40 dB
   54 Hz notch, broad/multiple harmonics, narrow cuts moving across half-bin
   offsets, alternating shelves every hop, per-channel opposing targets,
   silence/release, and sample rates 44.1/48/96/192 kHz. Test H256/128/64.
4. Every realized endpoint must separately pass cap excess <=0.01 dB, ripple
   <=0.01 dB, and the existing static response error <=0.25 dB. Every admitted
   pair must pass actual phase separation <=phi_limit and actual all-t cap
   excess <=0.05 dB. State the dense/refined or certified frequency coverage.
   Do not loosen a bound to compensate for finite truncation.
5. Renderer versus a literal time-varying direct sum <1e-10, including every
   P offset, non-host-aligned boundaries, full history wrap, new-bank overlap,
   arbitrary host chunks, and full tail decay. Verify zero extra buffer delay.
6. Small-FIR root checks across blend values; actual full-length endpoint and
   transition magnitude/phase gates with independent frequency evaluation.
7. Counters prove one target acquisition and at most one new IR per lane per
   actual H, no queue/repeated synthesis, and no stale target generation.
   Report fixed and moving target settling/error statistics and effective-cap
   lag. C++ complete-path timing remains a later Stage 3 acceptance gate.

Any failed candidate stays in the rejection ledger. Passing static primitives
or a phase-only calculation cannot substitute for these transition gates.
