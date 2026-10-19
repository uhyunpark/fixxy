# Verified spectral control C++ port

Date: 2026-09-12. Scope: `src/dsp/SpectralControl.h` and
`test/spectral_control_test.cpp`; additive `SpectralControlTest` CMake target and
pre-commit hook entry. This is our designed approximation. It does not establish
soothe sonic equivalence and is not yet connected to the production processor.

## Ported behavior

The source is the verified control section of `proto/spectral_parity.py` and the
generated `test/spectral_control_fixtures.h`. The header provides both detector
knees; all six Detail/attack/release tilts; Detail width, strength and detector
pole; normalized timing maps; the two-state decoupled reduction follower;
power-domain stereo linking, orthonormal M/S, focus factors, parallel bandpass
masks, range skirts, FFT amplitude normalization, and cap smoothing.

`FrequencyWindows` uses physical bin frequencies. Its exact linear-amplitude
median covers each destination's +/-0.5 octave interval, includes both frequency
boundaries, averages the two central values for even counts, and passes DC
through. Sorting amplitude indices once per frame supplies exact ranks; a
Fenwick count tree adds/removes bins as that interval slides and selects the
median in bounded logarithmic work. Duplicate amplitudes have deterministic
index tie-breaking. This is numerically identical to the Python direct median,
not a histogram approximation or the existing log-grid median.

Detail smoothing uses precomputed destination-specific intervals and a prefix
sum. It is a normalized box with the exact verified width convention, including
clipped upper-frequency windows. The width map can be updated without allocating.
The median requires distinct input/output storage; the box smoother supports
in-place operation. Input arrays must be finite, frequencies strictly increasing
and nonnegative, and parameter ranges validated by their owner.

## Numeric verification

The initial test-first RED compile failed because `dsp/SpectralControl.h` did
not yet exist. The completed port passes the generated fixtures, independently
evaluated pole step responses, direct finite-window references, routing
identities, silence/missing-source demand, depth zero, cap enforcement, and
six-tilt independence checks.

- Combined C++ numeric maximum: `1.87627691162e-14`.
- Generated fixture/maps and closed-form state checks before the dense direct
  check were added: maximum `1.06581410364e-14`.
- Tracked prepared-operation C++ allocations/deletions: `0 / 0`.
- AddressSanitizer + UndefinedBehaviorSanitizer: PASS, no diagnostics.
- Clean direct Clang builds and the CMake target build: PASS, zero warnings with
  `-Wall -Wextra -Wconversion -Wshadow`.

The C++ `--dump` path emits the actual frequencies, widths, inputs, median and
box output. A separate Python process compares those arrays to the **verified**
`octave_median` and `variable_box_smooth_direct`, avoiding a self-consistency-only
test of the optimized implementation:

| Scenario | Bins / sample rate | Exact median max error | Direct weighted box max error |
|---|---|---|---|
| 0 | 2049 / 48 kHz | 0 | 1.8984813721090177e-14 |
| 1 | 513 / 44.1 kHz | 0 | 6.6613381477509392e-15 |
| 2 | 4097 / 96 kHz | 0 | 2.2093438190040615e-14 |
| 3 | 8193 / 192 kHz | 0 | 3.0420110874729289e-14 |
| 4, repeated amplitudes | 2049 / 48 kHz | 0 | 3.5110803153770576e-15 |

All independent comparisons pass `1e-12`. The console test also carries a
literal sorted-window median and weighted-sum reference for all 2049 destinations,
and verifies silence, DC pass-through, and in-place box processing. The timing
tests use actual `H=64/128/256` at 44.1/48/96/192 kHz, compare detector and attack
to elapsed-time exponential responses, and compare both stages of release to
the independently evaluated closed-form two-pole response. Calibration checks
distinguish DC/Nyquist from positive interior FFT bins.

Reproduction:

```sh
cmake --build build --target SpectralControlTest -j 2
build/SpectralControlTest
clang++ -std=c++20 -O3 -Wall -Wextra -Wconversion -Wshadow -Isrc test/spectral_control_test.cpp -o /tmp/spectral-control-test
/tmp/spectral-control-test --benchmark
clang++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wconversion -Wshadow -Isrc test/spectral_control_test.cpp -o /tmp/spectral-control-sanitized
/tmp/spectral-control-sanitized
proto/.venv/bin/python - <<'PY'
import sys, subprocess, io
import numpy as np
sys.path.insert(0, 'proto')
import spectral_parity as sp
for case in range(5):
    x = np.loadtxt(io.StringIO(subprocess.check_output(
        ['build/SpectralControlTest', '--dump', str(case)], text=True)))
    median_error = np.max(np.abs(x[:, 3] - sp.octave_median(x[:, 2], x[:, 0])))
    box_error = np.max(np.abs(x[:, 4] - sp.variable_box_smooth_direct(x[:, 2], x[:, 0], x[:, 1])))
    assert median_error < 1e-12 and box_error < 1e-12
    print(case, median_error, box_error, 'PASS')
PY
```

## Measured control-frame budget

Apple M1, Clang `-O3`, N4096 at 48 kHz, 20 warmup frames followed by 100 measured
frames. Channels have separate detector and reduction state. Each measured
channel performs exact median, Soft exceedance/pole/knee, variable-width box,
and decoupled ballistics. Four channels represent main plus sidechain detector
workload where both sources are required. Scratch storage is reused serially.

| Hop | Channels | Mean microseconds | Sampled max microseconds | Hop budget microseconds |
|---|---|---|---|---|
| 256 | 1 | 197.212 | 234.875 | 5333.333 |
| 256 | 2 | 277.500 | 315.667 | 5333.333 |
| 256 | 4 | 542.486 | 573.125 | 5333.333 |
| 128 | 1 | 140.113 | 147.333 | 2666.667 |
| 128 | 2 | 271.529 | 292.541 | 2666.667 |
| 128 | 4 | 566.290 | 595.458 | 2666.667 |
| 64 | 1 | 144.908 | 153.417 | 1333.333 |
| 64 | 2 | 281.246 | 290.166 | 1333.333 |
| 64 | 4 | 564.963 | 586.042 | 1333.333 |

These are sampled control-only timings, not hard worst-case guarantees. They
exclude analysis FFT, rendering/FIR synthesis, coefficient/width recomputation,
and processor/host overhead. Stereo consumes about 21.1% of the H64 budget on
average in this run; four detector channels consume about 42.4%. Larger
sample-rate-scaled grids have numeric coverage but are not covered by these
performance figures. Coefficient changes can add meaningful frame cost and must
be measured during final automation testing.

## Explicit audio-thread audit

No production `processBlock()` was edited. For these candidate audio-thread
primitives, `prepare()` is the only allocating method; it sizes all vectors and
precomputes intervals. After preparation, median/smoothing, width updates,
coefficient evaluation, knees, routing and ballistics do not allocate, delete,
lock, perform I/O, log, or use atomics/JUCE. Per-bin formulas are constant work;
median is O(B log B), smoothing O(B), and width setup O(B log B). Sorting uses
bounded O(log B) stack recursion on this standard library. No asynchronous
ownership transfer or deallocation occurs in the control path. Instrumented
`new`/`delete` checks cover prepared median, smoothing and width updates;
scalar formulas/state are inspected directly. All state/coefficient math is
double precision.

## Integration prerequisites

The owner must smooth every user parameter before applying it, recompute
coefficient arrays and intervals outside the sample loop, and retain separate
Soft detector, release, and output state for every channel/bin. Parameter
publication and ownership remain the plugin layer's responsibility. This port
does not introduce a graph, scheduler, mode transition, or FIR transition policy.

Apply masks after sensitivity and frequency smoothing. Apply the final cap after
every temporal update, using the 20 ms smoothed cap; disabled Max cut means the
40 dB design maximum. `Ballistics::clamp` preserves the prototype's output-only
clamp. Internal release memory is retained; it cannot exceed the output promise
when the final clamp is applied every frame. Clamping that internal memory too
would change recovery behavior and requires an additional prototype test.

Silence/missing external sidechain passes `active=false` to the knee, then feeds
zero demand through the existing reduction follower; it never falls back to
main input. Passing zero exceedance to Soft is insufficient because its baseline
knee output is nonzero. Reset/release policy for detector state during mode or
sidechain changes must be specified by the integrating node.

The control math can also be used with the existing N1024 WOLA renderer as an
interim integration. Hard detection must use `2*abs(X)/sum(actual_analysis_window)`
at positive interior bins and half that at DC/Nyquist; synthesis/COLA scaling is
not analysis amplitude calibration. The square-root Hann/N1024 and proposed
periodic Hann/N4096 detectors have different tone leakage and noise-bin
statistics. Final-audio transfer calibration is required before reusing the
frozen Soft knee tuning. This port makes no analysis-window-independent sonic
claim. A real detector pipeline and end-to-end audio tests remain necessary.

Pluginval strictness 5, AU validation, and visual signal checks are product
integration gates, not evidence for these unconnected primitives. This bounded
agent did not run pluginval or change plugin/UI behavior; the lead owns the
final product checks. No commit was made.
