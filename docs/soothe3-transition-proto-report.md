# Phase-limited FIR transition — Stage 2 report

Date: 2026-09-12. Source specification:
`docs/soothe3-transition-revision.md`. Implementation:
`proto/spectral_transition.py`. This is our approximation, not evidence of
soothe3's algorithm or sonic equivalence. The numerical transition gate passes;
C++ real-time throughput and product integration remain separate gates.

Exact gate command, run from the repository root:

`proto/.venv/bin/python proto/spectral_transition.py`

Fixture regeneration command:

`proto/.venv/bin/python proto/dump_spectral_transition_fixtures.py`

## Decisions and rejection ledger

- The original endpoint-only crossfade remains rejected: its opposing 40 dB
  shelves reached `50.3801383123 dB` excess over the cap.
- The queued five-segment workaround remains rejected despite its numerical cap
  result because its 320-sample duration discards High/Ultra admission cadence.
- The revised nominal design-grid phase limit also fails as predicted by Stage
  1. Across the complete moving/cap/sample-rate suite, actual truncated-FIR
  phase exceeded the `0.191793750275 rad` limit by `0.00287776679601 rad`.
  Preliminary `1e-5` and `1e-3 rad` reserves were also rejected as the suite
  expanded; thresholds were not loosened.
- The selected fixed admission reserve is `0.01 rad`, leaving a grid admission
  limit of `0.181793750275 rad`. This is 3.47 times the measured nominal
  finite-FIR discrepancy and consumes 5.21% of the proof's phase budget. It is
  fixed—not adaptively tuned or subdivided—and passed the full gate below.

## Formula and oracle gates

The retained state is the unwrapped causal-cepstrum log spectrum, not the log of
truncated taps. Each actual Quality hop acquires the latest target once and
synthesizes at most one admitted IR:

`alpha=min(1, (phiLimit-0.01)/max(abs(imag(ellTarget-ellCurrent))))`.

`ellNext=ellCurrent+alpha*(ellTarget-ellCurrent)`, with real DC/Nyquist, and
`Cnext=(1-alpha)*Ccurrent+alpha*Crequest`. Fade weights remain
`0.5-0.5*cos(pi*q/H)` for `q=0..H-1`; the next boundary supplies the exact
endpoint.

The exact cap-normalized quadratic extremum implementation evaluates endpoints
and every real stationary root in `[0,1]`. Against independent
`scipy.optimize.minimize_scalar` its worst discrepancy was
`8.47906293788e-12 dB`; against a 100001-point direct `t` sweep it was
`8.47809669691e-12 dB`. Unequal magnitudes, caps 0/6/12/20/40 dB, zero and
positive/negative phase-limit extremes gave cosine-bound violation
`5.55111512313e-17` and the constructed maximum allowed overshoot exactly
`0.05 dB`.

The old falling-cap rule was independently reproduced:

- `H=256`: `0.650865458564 dB`
- `H=128`: `0.390417354619 dB`
- `H=64`: `0.215279007017 dB`

The reproduction differs from the Stage 1 values by at most
`3.61006780025e-11 dB`. The corrected published effective-cap trajectory had
`1.44649119983e-15 dB` maximum excess. During repeated falling retargets the
requested and effective values differed by as much as `2.19276250893 dB` and
converged to within `0.000792509439 dB` for the fixed request. The UI/audio
guarantee must therefore publish the effective cap, not only the knob or 20 ms
control-smoother value.

## Actual finite-FIR coverage and maxima

Coverage used M/L `(16384,4096)`, `(32768,8192)`, and `(65536,16384)` as already
verified for 44.1/48, 96, and 192 kHz; all `H=256/128/64`; caps
6/12/20/40 dB; unity/deep cuts, opposing shelves, 40 dB/54 Hz notch, broad and
multiple-harmonic curves, half-bin moving notches, alternating targets, the same
opposing-target cases applied independently to each render lane, and unity/
silence release. Frequency evaluation used at least
65537 uniform points and at least four points per design-grid interval, so every
design bin and midpoint was covered. Global dense phase/magnitude maxima were
then locally refined with bounded scalar minimization. This is dense/refined
numeric evidence, not a derivative-bound continuum certificate.

- actual phase separation: `0.184564380841 rad`
- remaining phase margin: `0.00722936943393 rad`
- endpoint cap excess: `0 dB`
- endpoint above-unity ripple: `1.15719295986e-14 dB`
- endpoint response error: `0.0584794266559 dB`
- exact all-t effective-cap excess on actual pairs: `0 dB`
- smallest admitted alpha: `0.0558325686806`

All remain below `phiLimit`, `0.01`, `0.01`, `0.25`, and `0.05` respectively.
The test counted 180 latest-target acquisitions and exactly 180 synthesized IRs;
target age at acquisition was always zero, with no queue or stale generation.

The phase slew is intentionally visible. Moving targets differed from their
latest requested reduction by up to `37.7666972528 dB`, and the effective cap
lagged a new requested cap by up to `25.5207224362 dB`. A fixed opposing-shelf
move settled in exactly the predicted 20 hops: about 106.7/53.3/26.7 ms at
48 kHz for Normal/High/Ultra. The candidate is safe under the tested bound; it
does not claim instant spectral tracking or turn the 1 ms detector pole into a
1 ms end-to-end response.

## Renderer/history and minimum-phase checks

A bank activated after wrapping its `J+1` history ring recomputes both current
and prior overlap blocks with its own coefficients. Across H=256/128/64,
arbitrary nonaligned host chunks, a non-host callback admission edge, all tail
decay, and direct-head processing:

- time-varying direct-sum error: `5.89805981832e-17`
- newly activated bank versus a new kernel warmed from the beginning: `0`
- q=0/next-boundary endpoint error: `3.12250225676e-17`
- added buffer delay: `0 samples`

A separate small-FIR homotopy probe checked 1001 blend weights: maximum zero
radius `0.160128153805`, NumPy/SciPy root disagreement `0`, minimum unit-circle
magnitude `0.69`, and positive first taps throughout. This numerically supports
the Stage 1 root-continuity deduction without using ill-conditioned 4096th-order
root finding.

## Fixture and remaining gates

`proto/dump_spectral_transition_fixtures.py` reruns every gate before generating
`test/spectral_transition_fixtures.h`. The scoped header pins phase constants,
one falling-cap spectral admission, its next reduction/taps, and an activated
bank history/output fixture. Static FIR and control fixtures remain separate.

This pass does not provide a derivative-bound certificate over the mathematical
continuum, a perceptual/listening verdict, reference-plugin calibration, or an
audio-thread throughput result. Its locally refined dense grids are empirical
numeric coverage of the specified suite.

The next stage must still measure the complete three-transform synthesis path,
partition preparation, new-bank history reconstruction, detector work, and four
render lanes at every sample-rate/hop deadline. Passing Python math does not
certify an audio deadline. No C++ production/schema files were edited and no
commit was made.
