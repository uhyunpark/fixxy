# Phase-limited FIR transition C++ port

Completed 2026-09-22. Numerical primitive milestone: PASS. Full production
throughput: **not approved**. At 48 kHz, every-hop parameter automation exceeds
the H64 deadline; every tested 96/192 kHz cadence exceeds the mean deadline.
The implementation remains outside `SpectralNode`, processor, schema and UI.
No commit was made.

## Verified scope and API

`SpectralTransition.h` ports the passed `proto/spectral_transition.py` policy.
It retains the causal-cepstrum log spectrum before exponential/truncation and
admits the latest supplied target with
`alpha=min(1,(0.19179375027524279-0.01)/max(abs(delta.imag)))`, with alpha=1 for
zero phase distance. The fixed 0.01 rad reserve is mandatory. The rejected
nominal-limit default in the Python helper is **not** used.

`prepare(M,L,H,initialReduction,initialCap)` allocates one mono lane. The owner
calls `beginHop(latestReduction,smoothedRequestedCap)` exactly at H boundaries,
then splits arbitrary callbacks into `process` calls of at most `remaining()`
samples. Early admission returns false without altering state. Each accepted
hop computes one target causal log (two M FFTs), one admitted endpoint (one M
IFFT), one partition refresh, and one bank history reconstruction. There is no
subdivision queue, worker, stale-target retention policy or repeated synthesis.
Freshness of the supplied pointer remains the integrating owner's responsibility.

Two prepared banks always receive identical input. Their active/inactive roles
swap when the H-sample fade completes. The new bank recomputes the previous
overlap **and** current tail with its own coefficients using the retained J+1
input spectra. Head history is retained. `q=0..H-1` uses
`0.5-0.5*cos(pi*q/H)`; the next hop's q=0 supplies the exact previous endpoint.
Input/output aliasing is supported. There is zero additional buffer delay.

Effective cap follows the admitted convex log endpoint, then the identical
raised-cosine fade. It is intentionally distinct from the knob and independent
20 ms requested-cap smoother. `effectiveCap()` refers to the next output
sample's fade position; after processing a complete hop it gives the admitted
endpoint cap. A UI must publish that value at the matching audio position.

Minimal `SpectralFir.h` extensions separate causal-log creation from admitted
synthesis, retain a prepared partition FFT for allocation-free kernel refresh,
and expose exact partition-boundary history reconstruction. Existing static
primitive tests remain unchanged and pass. `SpectralTransitionTest` and
`SpectralTransitionScalarTest` are registered in CMake and the pre-commit hook.

## Numerical checks and boundaries

Test-first RED was a compile failure for the then-missing
`dsp/SpectralTransition.h`. After the port, both backends pass with unchanged
`1e-10` numeric gates and zero warnings under
`-Wall -Wextra -Wconversion -Wshadow`.

| Check | vDSP maximum | Scalar maximum |
|---|---:|---:|
| Generated admission alpha/cap/reduction/taps combined | 2.48689957516e-14 | 4.26325641456e-14 |
| Generated history fixture | 6.93889390391e-17 | 6.93889390391e-17 |
| Newly activated bank vs kernel warmed from sample zero | 0 | 0 |
| Repeated H64/128/256 dynamic direct-sum oracle | 1.38777878078e-15 | 1.38777878078e-15 |
| Activation impulses at all 64 partition offsets | 9.64939933512e-18 | 1.13841228111e-17 |
| Falling effective-cap excess | 7.1054273576e-15 dB | 7.1054273576e-15 dB |

Dynamic tests process 8192 signal samples plus the complete 4096-sample tail,
through host chunks 1/17/91/3/511/64/205/7/129. Latest opposing shelves, a
half-bin 54 Hz notch and unity replace each other every H, while requested caps
alternate 6/12/40 dB. There are exactly 192/96/48 admissions at H64/128/256,
respectively, with full history-ring wrap. The independent time-domain oracle
uses a literal convolution sum for both actual kernels and the exact fade
weight. Separate history tests pin generated arbitrary FIR fixtures, warmed-bank
identity, and pre-boundary/during-fade impulses at every input offset.

Repeated scalar falling-cap tests distinguish the requested/effective values:
maximum gaps are 2.19276250892/4.24410715254/7.9584364956 dB at H64/128/256.
The slowest final settling error after 160 hops is 0.00079250943886 dB, below
the separately stated 0.001 dB settling gate; it is not a numerical port error.

An independent Python process compared 12 changing C++ admissions per backend
at 48/96/192 kHz against the verified Python causal-log/admission functions with
the fixed reserve. Across all 72 admissions:

| Field | vDSP maximum | Scalar maximum |
|---|---:|---:|
| Alpha | 7.77156117e-16 | 5.55111512e-16 |
| Admitted cap | 1.42108547e-14 | 1.42108547e-14 |
| Unwrapped phase distance | 1.77635684e-15 | 8.88178420e-16 |
| Every finite tap | 2.22044605e-16 | 2.22044605e-16 |

AddressSanitizer plus UndefinedBehaviorSanitizer PASS with no diagnostics.
Tracked ordinary C++ allocations/deletions across prepared updates and dynamic
rendering are 0/0. The previous static FIR test suite passes both backends after
the extensions: fixture taps <=1.56e-17, renderer <=1.81e-15, all impulse offsets
<=1.39e-16, and heap 0/0.

Stage 2's dense/refined actual finite-FIR phase maximum remains
0.184564380841 rad versus 0.191793750275 rad, with endpoint/all-t cap gates
passed. The C++ fixtures and independent changing-admission checks establish
the port matches that verified construction. This is numerical evidence for
the tested suite, **not** a derivative-bound continuum certificate or proof
for every possible future curve. No soothe sonic-equivalence claim is made.

## Complete scoped path timing

Apple M1, Clang `-O3 -DNDEBUG`, stereo: two analysis/control lanes and four
active render banks. Each row contains 20 warmup hops then 100 measured hops;
p99 is the nearest-rank 99th of 100 observations. Times below are microseconds.
Mean/median/p99/max are wall-clock samples, never hard worst-case guarantees.

The measured interval includes both window multiplies and analysis FFTs,
calibrated magnitudes, exact octave medians, Soft detection/knees, spatial
smoothing, decoupled ballistics, cap smoothing/clamping, target interpolation,
both three-transform admissions, both partition refreshes, both new-bank
history reconstructions, and four-bank direct-head/tail rendering. Analysis N
is 4096/8192/16384, design M is 16384/32768/65536, and taps L are
4096/8192/16384 at 48/96/192 kHz. H remains the actual 256/128/64 samples;
the deadline is H/fs. Interpolation is only benchmark plumbing, not a new
production algorithm.

Automation rows additionally smooth Detail, attack, release, and all six tilts,
recompute every per-bin coefficient and width, and refresh all width intervals
on every measured hop. Requested cap moves in every run. Static rows hold
those nine controls fixed while analysis targets and cap continue changing.
The measurement excludes host/UI overhead, additional sidechain analysis,
curve-band coefficient updates, monitoring and mode/latency transitions.

The 48 kHz vDSP static rows are retained from September 16. The remaining rows
are the September 22 coordinated run with other agents' heavy workloads
paused. CPU frequency/scheduling can differ across dates and rows. An initial
September 22 run overlapped a Python material test and is not the primary
comparison: it saw up to 115638.459 us vDSP and 71536.000 us scalar maxima.
The coordinated rerun below also retains scheduling outliers; no failing
observation was removed to declare a passing deadline.

### vDSP, fixed control coefficients

| Rate | H | Mean | Median | p99 | Max | Deadline |
|---|---:|---:|---:|---:|---:|---:|
| 48 kHz | 256 | 787.305 | 787.000 | 797.042 | 798.250 | 5333.333 |
| 48 kHz | 128 | 744.779 | 744.354 | 754.166 | 759.958 | 2666.667 |
| 48 kHz | 64 | 724.292 | 723.646 | 738.500 | 758.791 | 1333.333 |
| 96 kHz | 256 | 2890.706 | 2799.146 | 3423.875 | 3430.208 | 2666.667 |
| 96 kHz | 128 | 2602.945 | 2579.979 | 2706.500 | 2720.417 | 1333.333 |
| 96 kHz | 64 | 2548.005 | 2530.562 | 2651.958 | 2660.125 | 666.667 |
| 192 kHz | 256 | 5806.185 | 5768.583 | 6209.833 | 6370.125 | 1333.333 |
| 192 kHz | 128 | 5644.243 | 5607.791 | 5914.791 | 5916.666 | 666.667 |
| 192 kHz | 64 | 5558.175 | 5523.000 | 5894.334 | 6162.458 | 333.333 |

### vDSP, all nine controls automated each hop

| Rate | H | Mean | Median | p99 | Max | Deadline |
|---|---:|---:|---:|---:|---:|---:|
| 48 kHz | 256 | 2169.320 | 2219.458 | 2763.125 | 2914.041 | 5333.333 |
| 48 kHz | 128 | 2193.022 | 2152.208 | 2667.458 | 2796.625 | 2666.667 |
| 48 kHz | 64 | 2230.442 | 2115.396 | 2473.584 | 2494.834 | 1333.333 |
| 96 kHz | 256 | 4283.678 | 4266.688 | 4381.250 | 4497.959 | 2666.667 |
| 96 kHz | 128 | 4630.073 | 4697.479 | 4914.792 | 4966.458 | 1333.333 |
| 96 kHz | 64 | 7311.247 | 5519.584 | 29039.833 | 32800.208 | 666.667 |
| 192 kHz | 256 | 14324.180 | 12702.459 | 37820.708 | 43226.292 | 1333.333 |
| 192 kHz | 128 | 8954.114 | 8898.584 | 9443.375 | 9483.417 | 666.667 |
| 192 kHz | 64 | 8869.659 | 8829.813 | 9335.625 | 9388.083 | 333.333 |

### Scalar, fixed control coefficients

| Rate | H | Mean | Median | p99 | Max | Deadline |
|---|---:|---:|---:|---:|---:|---:|
| 48 kHz | 256 | 3000.015 | 2998.146 | 3042.667 | 3051.292 | 5333.333 |
| 48 kHz | 128 | 2946.791 | 2930.563 | 3252.083 | 3311.625 | 2666.667 |
| 48 kHz | 64 | 2902.569 | 2893.500 | 3010.250 | 3092.583 | 1333.333 |
| 96 kHz | 256 | 7011.357 | 6982.541 | 7422.667 | 7515.042 | 2666.667 |
| 96 kHz | 128 | 6897.434 | 6886.292 | 7049.125 | 7083.500 | 1333.333 |
| 96 kHz | 64 | 6812.608 | 6796.812 | 6964.209 | 6980.500 | 666.667 |
| 192 kHz | 256 | 14902.140 | 14868.417 | 15923.000 | 16030.542 | 1333.333 |
| 192 kHz | 128 | 14817.561 | 14650.416 | 18512.375 | 18944.167 | 666.667 |
| 192 kHz | 64 | 14704.487 | 14703.146 | 15298.375 | 15373.959 | 333.333 |

### Scalar, all nine controls automated each hop

| Rate | H | Mean | Median | p99 | Max | Deadline |
|---|---:|---:|---:|---:|---:|---:|
| 48 kHz | 256 | 3767.652 | 3763.312 | 3849.500 | 3906.333 | 5333.333 |
| 48 kHz | 128 | 3683.640 | 3682.376 | 3718.916 | 3722.417 | 2666.667 |
| 48 kHz | 64 | 3647.470 | 3644.167 | 3684.291 | 3700.583 | 1333.333 |
| 96 kHz | 256 | 8616.581 | 8577.334 | 9114.167 | 9357.750 | 2666.667 |
| 96 kHz | 128 | 8512.627 | 8476.729 | 8867.833 | 9126.083 | 1333.333 |
| 96 kHz | 64 | 8818.967 | 8530.062 | 9856.166 | 9953.583 | 666.667 |
| 192 kHz | 256 | 25975.781 | 24703.020 | 40244.833 | 44555.708 | 1333.333 |
| 192 kHz | 128 | 28644.658 | 21565.646 | 53764.417 | 53892.083 | 666.667 |
| 192 kHz | 64 | 21734.938 | 17982.125 | 43126.958 | 49324.167 | 333.333 |

Only 48 kHz/H256 remains below the sampled deadline with every-hop automation
on both backends in this run. Even that is not a full product deadline proof.
vDSP 48 kHz/H128 has mean headroom but observed p99/max misses. H64 automation
and all higher-rate cadences fail average throughput. Moving synthesis to a
worker does not repair a workload whose average exceeds its target cadence.
No supported-rate restriction, cadence reduction, dropped update policy or
quality-label change has been silently introduced. These need a separate
implementation/measurement decision before production integration.

## Explicit audio-thread audit

No `processBlock()` was modified. Every vector, FFT setup, weight table and
buffer is sized in `prepare()`; destruction remains off-thread. `beginHop`,
kernel refresh, boundary replacement and `process` perform no C++ allocation,
deletion, lock, I/O, logging or atomics, and all arithmetic/state is double.
The fixed-capacity update is three M transforms per lane, J fixed-size partition
FFTs, and two bounded tail reconstruction sums/IFFTs. Rendering uses a bounded
64-tap direct head and partition sums every P=64. Fade weights are precomputed,
so no trigonometry occurs per rendered sample. Both banks retain their own
prepared histories; no ownership move/free occurs at the swap.

The runtime intercept covers ordinary C++ new/new[] and sized/unsized delete,
not Apple's internal C malloc/free. Source inspection confirms the vDSP setup
is created/destroyed only in preparation/destruction and transform scratch is
preallocated. Positive CPU bounds from fixed capacities do not establish that
the callback deadline is met; the failed complete-path timing gate above is
explicitly retained.

## Reproduction and remaining integration gates

```sh
cmake --build build --target SpectralTransitionTest SpectralTransitionScalarTest SpectralFirTest SpectralFirScalarTest -j 2
build/SpectralTransitionTest
build/SpectralTransitionScalarTest
build/SpectralFirTest
build/SpectralFirScalarTest
clang++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wconversion -Wshadow -Isrc test/spectral_transition_test.cpp -framework Accelerate -o /tmp/spectral-transition-test
clang++ -std=c++20 -O3 -DNDEBUG -DSPECTRALFFT_FORCE_SCALAR=1 -Wall -Wextra -Wconversion -Wshadow -Isrc test/spectral_transition_test.cpp -o /tmp/spectral-transition-scalar
/tmp/spectral-transition-test --benchmark
/tmp/spectral-transition-test --benchmark-automation
/tmp/spectral-transition-scalar --benchmark
/tmp/spectral-transition-scalar --benchmark-automation
clang++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wconversion -Wshadow -Isrc test/spectral_transition_test.cpp -framework Accelerate -o /tmp/spectral-transition-sanitized
/tmp/spectral-transition-sanitized
```

`--dump-sequence 1`, `2`, or `4` exports 12 changing admissions at the three
sample-rate capacities. Each row is alpha, effective endpoint cap, unwrapped
grid distance, then all taps. The independent Python reference starts from
unity/40 dB, cycles `low_shelf40/high_shelf40/halfbin_a/unity` from
`make_targets`, cycles caps 6/12/40, and calls
`admit_one_hop(current,target,ADMISSION_PHASE_LIMIT)` for each row. This is the
comparison that produced the cross-language table above.

Product gates remain separate: supported sample-rate/cadence policy, CPU
optimization, control/audio transfer calibration, phase/mode behavior, latency
notification, actual effective-cap publication, sidechain/monitor routing,
real-signal visual checks, and complete host validation. Pluginval strictness
5 and AU validation are owned by the lead for the product; this primitive
agent did not rerun them and cannot cite prior product validation as proof of
this unconnected transition engine's real-time suitability.
