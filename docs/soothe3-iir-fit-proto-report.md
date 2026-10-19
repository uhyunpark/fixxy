# Bounded IIR static-fit prototype: rejected at Stage 2A

2026-10-18. Prototype of `soothe3-iir-renderer-spec.md` sections 1–2 only.
No C++, product, CMake or fixture edits; no commits. The specified fitter/bank
candidate fails the unchanged **0.25 dB maximum-error** requirement on the first
independent narrow Gaussian. Stage 2B is not authorized by this result.

## Reproduction and bounded scope

Run from the repository root:

```
./proto/.venv/bin/python proto/spectral_iir_fit.py
```

48 kHz; K16 then K24, with two slots reserved for disabled endpoint shelves.
All six runs use the same global C=40 dB cap. The 20 dB Gaussian is therefore
tested as a 20 dB demand under that cap, not as a separate C=20 dB cap test.
This is an analytic-target/source-interpolation experiment, not a claim about
FFT detector resolution. Each source has 8192 unique samples: DC plus 8191
positive logarithmic frequencies from 20 Hz through Nyquist. Targets between
positive samples use log-frequency interpolation; DC stays explicit.

The fitting grid has 513 audible log samples, DC/Nyquist, and source peaks
plus half-height crossings; there are 518/527/518 merged fitting samples for
single/four/Gaussian targets. Peak retention uses one source scan and bounded
insertion of at most 64 peaks. Signed residual, largest-positive peak selection,
three clamped Q choices, exactly eight depth bisections, nine local scoring
probes with the prescribed overcut penalty, and the 0.05 dB positive-residual
stop are implemented. No optimizer, boost section, section-count expansion,
tail repair, or alternate fitter is used in fitting. Depth monotonicity is
checked at 33 bounded points before each bisection solve.

Coefficients call the existing verified `matched_kernel.ivantsov_2nd` unchanged,
with sigma `2.0283139124815617` from `MatchedCoeffs.h`. Zero depth produces
literal identity coefficients. The coefficient formulas were not rederived.
The optional flat attenuation is the minimum over the entire source grid;
it is zero for the decisive Gaussian. Shelves, cap backoff, and nonzero flat
attenuation are not validated by this initial screen.

## Results

Both K values produced the identical fit for each target; increasing the
available bell count from 14 to 22 did not change these outcomes.

| Target | K | Active bells | Max error, dB | RMS error, dB | Outside-target overcut, dB | Verdict |
|---|---:|---:|---:|---:|---:|---|
| Matched single, 1 kHz/Q4/8 dB | 16/24 | 1 | 0.047297208 | 0.003896622 | 0.000054129 | PASS |
| Known matched four-bell bank | 16/24 | 9 | 0.080423259 | 0.051422011 | 0.009331963 | PASS |
| Independent Gaussian, 3 kHz/54 Hz FWHM/20 dB | 16/24 | 6 | **2.735620272** | 0.187320403 | **1.745289824** | FAIL |

The four-bell control has `(f Hz, Q, cut dB)` = `(120,6,4)`, `(700,8,6)`,
`(3000,10,5)`, `(12000,5,3)`. Its target is the exact matched-bank coefficient
response sampled onto the source grid. Positive controls establish that the
specified greedy fitter can recover representative realizable targets within
the required budget; they do not prove optimality for every realizable bank.

“Outside-target” here means interpolated target reduction <=0.01 dB, rather
than an asserted brick-wall mask. The independent target is
`20*exp(-4*ln(2)*((f-3000)/54)^2)` dB. Its worst fit error is at
3057.861328 Hz. Maximum actual attenuation is 22.091259380 dB; fitted peak
2999.636529 Hz versus interpolated target peak 3000.095850 Hz. Fitted midpoint
width is 50.019847 Hz versus interpolated target width 54.023639 Hz (analytic
width exactly 54 Hz). All target peaks have <=0.05 dB **positive** residual at
the retained peak samples, so the fitter stops after six bells rather than
hitting K. Earlier overcuts remain in the signed residual and cannot be undone
by adding cut-only bells. This explains why extra slots did not help this
specified algorithm; it is not a proof that every possible cut-bell fit fails.

Maximum source interpolation error versus the analytic Gaussian is
0.031809088 dB. Consequently even the lower bound on maximum error against
the underlying analytic target is 2.703811184 dB, decisively beyond 0.25 dB.
The two control interpolation errors are 0.000229354 and 0.003463218 dB.

At 0.5/1/2 dB diagnostic thresholds the Gaussian fits 65617/65979/66314 of
66573 validation samples (98.563982% / 99.107746% / 99.610953%). Both controls
fit all their validation samples at all three diagnostic thresholds. These
fractions do not change the maximum-error gate.

## Independent numerical checks

Every proposed final bank was evaluated on 65537 uniform linear frequencies
including DC/Nyquist, augmented by 257 log probes per section and bounded
local attenuation-extremum searches around each center. Total unique validation
samples are 65796 / 67867 / 66573. RMS uses only the fixed 65537-point linear
grid so added local probes cannot alter its weighting. Optimizing local probe
positions is validation only and does not feed back into fitting.

Independent SciPy `signal.sosfreqz` versus direct coefficient-polynomial response
passes the assert at <=1e-10 dB. Worst differences are 3.1264e-13 / 4.4704e-11 /
3.6504e-12 dB for single/four/Gaussian. All coefficients are finite, b0>0;
all pole/zero roots have modulus <1, and denominator and normalized numerator
pass the real second-order Schur inequalities. Minimum pole/zero distance
inside the unit circle is respectively:

| Target | Pole margin | Zero margin |
|---|---:|---:|
| Single | 0.0256171740 | 0.0102223453 |
| Four | 0.0016158242 | 0.0010216554 |
| Gaussian | 0.0015117264 | 0.0007864573 |

An independent analog transfer check at 100 Hz/Q4/8 dB cut over 10–1000 Hz
has maximum difference 0.000045983 dB. No assertion equates matched and RBJ
responses at high frequency.

Numerically sampled C=40 dB cap excess is zero in every run. Largest sampled
above-unity ripple is 2.4384e-11 dB, below the 0.01 dB gate; all use depth scale
1, so no cap-backoff retry is exercised. These are numerical observations,
**not a continuum certificate**. Outward-rounded interval certification, its
error bound, failure rate and timing remain pending because the fit gate has
already failed. No performance benchmark ran concurrently with the owner's
build. No real-time or universal time-varying stability claim follows.

## Stop decision

Reject the **specified bounded fitter/bank candidate under the current target
contract**. The positive controls pass, so this is a demonstrated required-target
fit failure rather than a coefficient implementation failure. A universal claim
that no K24 finite-bell bank could approximate this target would require another
argument and is not established here.

Per the early-stop gate, 40 dB Gaussian, 80/100 Hz pair, disjoint protected gaps,
harmonic/dense/broad/edge/DC/Nyquist cases and the six-rate matrix were not run.
The first two unrun independent target recipes remain in the prototype as
explicit queued cases. No dynamic scheduling, slot recycling, modulation test,
FIR renderer, C++ port or product control was implemented. Further work requires
the owner's decision on a different fitter/architecture or an explicitly revised
approximation contract; this report does not relax the existing tolerance.
