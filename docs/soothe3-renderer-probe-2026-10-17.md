# Existing FIR renderer: bounded capacity and timing probe

Probe run: 2026-10-18 KST (retains the 2026-10-17 audit filename).
Apple M1, Apple Clang 21.0.0, C++20, `-O3 -DNDEBUG`,
`-Wall -Wextra -Wconversion -Wshadow`. No DSP kernel, CMake, processor or UI edits.

## Decision

The existing FIR path is **not demonstrated feasible for full integration**.
The corrected sample-rate-scaled schedule still fails supported high-rate rows,
and half the desired high-rate schedules exceed the transition's fixed capacity.
This is a scoped stereo rejection, not a measurement of a future 16-channel design.
Do not expose minimum-phase, linear-phase, quality or low-latency controls on this evidence.

At 96 kHz/H256, vDSP automation mean 2621.978 us narrowly fits 2666.667 us,
but p99 2743.791 us and max 2753.750 us miss. At 192 kHz/H256, even fixed
controls average 3536.081 us against 1333.333 us; automation averages 5592.857 us.
Current vDSP 48 kHz/H64 automation samples fit (mean 1150.983 us, max
1221.833 us). The historical 2230 us result is not presented as this run's result.
Clock, compiler and scheduling differences prohibit treating cross-date changes
as an isolated optimization result.

## Method and capacity

`--benchmark-scaled` reuses the existing two control/analysis lanes, two
`SpectralTransition` lanes and four always-running render banks. Base hops
256/128/64 multiply by 1 at 44.1/48 kHz, 2 at 88.2/96 kHz, and 4 at
176.4/192 kHz. N/M/L remain 4096/16384/4096 times that multiplier.
Controls stable means the nine detail/timing/tilt controls stay fixed; detector
targets and the requested-cap smoother still change. Automation updates all
nine smoothed controls and all coefficient/width tables on every actual hop.

The transition only accepts H64/128/256 and owns 256-sample fixed arrays.
H512/1024 are explicitly printed as UNSUPPORTED and never executed. Removing
assertions would not enlarge those arrays and would be memory-unsafe.

| Sample rates | Supported actual H | Unsupported actual H |
|---|---|---|
| 44.1/48 kHz | 256, 128, 64 | none |
| 88.2/96 kHz | 256, 128 | 512 |
| 176.4/192 kHz | 256 | 1024, 512 |

Each supported row has 100 measured hops after max(20, L/H+2) warmup hops
(20/34/66 for base H256/128/64), filling the retained convolution history.
The original benchmark modes retain their original 20-hop warmup.
p99 is nearest-rank observation 99 of 100. Timings are steady-clock wall time
in microseconds, with no removed observations. A coordinated exclusive slot
paused other agents' builds and heavy workloads; vDSP and scalar were executed
sequentially after both compiles completed. Ordinary OS scheduling remains.

## Total path timing

All table entries are microseconds. Each stable/automated cell is
**mean / p99 / max**. Deadline is H/fs; this does not test callback burst deadlines.

### vDSP

| fs | H | Deadline | Stable | Every-hop automation |
|---:|---:|---:|---|---|
| 48000 | 256 | 5333.333 | 862.853 / 1355.334 / 1383.042 | 1202.568 / 1275.875 / 1341.416 |
| 48000 | 128 | 2666.667 | 700.103 / 768.417 / 801.834 | 1160.022 / 1204.292 / 1207.542 |
| 48000 | 64 | 1333.333 | 679.158 / 721.416 / 727.333 | 1150.983 / 1211.625 / 1221.833 |
| 96000 | 256 | 2666.667 | 1650.367 / 1805.375 / 1806.917 | 2621.978 / 2743.791 / 2753.750 |
| 96000 | 128 | 1333.333 | 1580.289 / 1731.125 / 1732.333 | 2541.613 / 2702.959 / 2754.250 |
| 192000 | 256 | 1333.333 | 3536.081 / 3686.459 / 3798.542 | 5592.857 / 5897.708 / 6019.333 |
| 44100 | 256 | 5804.989 | 735.934 / 771.667 / 776.166 | 1201.526 / 1290.625 / 1375.958 |
| 44100 | 128 | 2902.494 | 695.250 / 743.583 / 862.292 | 1160.855 / 1209.708 / 1242.709 |
| 44100 | 64 | 1451.247 | 678.197 / 708.500 / 733.209 | 1140.751 / 1195.750 / 1214.750 |
| 88200 | 256 | 2902.494 | 1648.798 / 1831.333 / 1891.334 | 2612.513 / 2717.166 / 2719.583 |
| 88200 | 128 | 1451.247 | 1626.768 / 1700.750 / 1732.333 | 2546.121 / 2643.666 / 2667.833 |
| 176400 | 256 | 1451.247 | 3534.494 / 3661.917 / 3691.041 | 5555.491 / 5808.916 / 5858.750 |

### scalar

| fs | H | Deadline | Stable | Every-hop automation |
|---:|---:|---:|---|---|
| 48000 | 256 | 5333.333 | 1885.314 / 2172.250 / 2282.667 | 2323.664 / 2390.166 / 2419.042 |
| 48000 | 128 | 2666.667 | 1805.682 / 1873.375 / 1876.125 | 2273.495 / 2355.917 / 2445.625 |
| 48000 | 64 | 1333.333 | 1789.895 / 1991.292 / 2000.375 | 2256.043 / 2328.792 / 2560.708 |
| 96000 | 256 | 2666.667 | 4278.870 / 4420.417 / 4425.125 | 5274.816 / 5547.666 / 5627.917 |
| 96000 | 128 | 1333.333 | 4192.920 / 4514.542 / 4700.792 | 5193.048 / 5985.292 / 6025.291 |
| 192000 | 256 | 1333.333 | 9299.371 / 10701.167 / 10863.500 | 11250.886 / 11571.916 / 11796.459 |
| 44100 | 256 | 5804.989 | 1853.330 / 1913.583 / 1924.500 | 2316.616 / 2392.792 / 2414.209 |
| 44100 | 128 | 2902.494 | 1816.222 / 1891.917 / 1915.583 | 2273.419 / 2423.916 / 2475.500 |
| 44100 | 64 | 1451.247 | 1789.323 / 1862.416 / 1879.000 | 2253.050 / 2340.541 / 2404.458 |
| 88200 | 256 | 2902.494 | 4256.737 / 4410.875 / 4544.875 | 5250.272 / 5496.750 / 5505.167 |
| 88200 | 128 | 1451.247 | 4184.087 / 4462.208 / 4465.792 | 5168.462 / 5406.167 / 5439.750 |
| 176400 | 256 | 1451.247 | 9186.190 / 9529.375 / 9577.083 | 11227.313 / 11486.000 / 11565.292 |

## Measured phase attribution

The benchmark's existing control helper was mechanically split into coefficient
setup and analysis methods; its original update method calls the same two
methods. No coefficient cache or interpolation change was adopted. Four timed
intervals cover cap/control coefficient setup; analysis, detector, ballistics
and target interpolation; `beginHop`; and four-bank render. Per-phase mean,
p99 and max are printed by the new mode after each row, outside timed work.

These selected automated rows show measured **mean / p99 / max**, in us:

| Backend, fs, H | Cap/coefficient setup | Analysis/detector/interpolation | Admission/synthesis/refresh/history | Four-bank rendering |
|---|---|---|---|---|
| vDSP, 48000, 64 | 467.212 / 494.583 / 496.500 | 264.257 / 290.250 / 291.416 | 398.448 / 425.666 / 436.334 | 21.065 / 24.166 / 30.042 |
| vDSP, 96000, 256 | 967.796 / 1017.167 / 1022.333 | 573.495 / 652.875 / 661.875 | 961.611 / 1013.209 / 1029.125 | 119.075 / 131.416 / 131.416 |
| vDSP, 192000, 256 | 2031.216 / 2152.292 / 2158.750 | 1180.544 / 1283.041 / 1383.125 | 2183.797 / 2356.667 / 2386.084 | 197.300 / 216.916 / 229.167 |
| scalar, 48000, 64 | 466.545 / 491.750 / 537.583 | 321.686 / 347.000 / 370.917 | 1442.557 / 1506.833 / 1626.333 | 25.256 / 27.125 / 30.458 |
| scalar, 96000, 256 | 971.830 / 1021.792 / 1053.208 | 715.236 / 802.208 / 807.083 | 3447.185 / 3644.042 / 3714.625 | 140.564 / 151.417 / 153.334 |
| scalar, 192000, 256 | 2024.807 / 2079.292 / 2099.167 | 1524.893 / 1636.208 / 1652.417 | 7473.162 / 7766.583 / 7927.709 | 228.023 / 257.500 / 267.958 |

In the automated vDSP rows, combined controls (setup plus analysis) dominate
the total; coefficient setup alone and the aggregate admission interval are
similar at higher rates. In every scalar row, the admission interval is the
largest measured phase. Four-bank rendering is a small fraction in both.
Under fixed controls, admission is the largest phase in both backends.

`beginHop` privately combines causal-log transforms, phase admission,
endpoint synthesis, partition refresh and history reconstruction. Their
individual costs are **unknown**, not inferred from this aggregate interval.
No kernel instrumentation or audio-thread logging was added. Even eliminating
vDSP coefficient setup entirely would leave 192 kHz automation's measured
remaining mean above H256's deadline. Coefficient caching alone cannot
establish full throughput for that supported row.

## Verification and next step

Both direct builds completed with zero warning diagnostics. Both original
console suites passed unchanged 1e-10 gates: vDSP maximum error
2.48689957516e-14, scalar 4.26325641456e-14; tracked heap allocations/deletions
0/0 in each. No new DSP algorithm, approximation, fixture or prototype was
introduced. All allocations for the optional benchmark remain before measured
loops; timing output occurs after each row. No processBlock code changed.

The next bounded experiment could verify a 100 Hz coefficient-update/interpolation
candidate against an independent direct-per-hop oracle and declared error budget,
but this is not yet an approved optimization and does not solve the high-rate
admission floor by itself. If a renderer replacement is selected, the smallest
scientifically justified alternative is a fixed-capacity dynamic minimum-phase
IIR notch/shelf bank, first researching fitting error, stable coefficients,
bounded peak selection and cap-safe transitions, then independent Python
verification before any C++ port. Dense arbitrary-target accuracy would be a
tradeoff requiring explicit measurement. The present proven WOLA engine keeps
its identity; it is not renamed minimum-phase.

Full processor callback bursts (32/64/128/256/1024 and odd blocks), target age,
sidechain, all bands, **16 unlinked channels**, routing, multiple instances and
mode transitions remain required before integration. Stereo hop samples do not
prove any of those gates; no production real-time approval is granted.

## Reproduction

```sh
clang++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wconversion -Wshadow -Isrc test/spectral_transition_test.cpp -framework Accelerate -o /tmp/soothe3-renderer-probe-vdsp
clang++ -std=c++20 -O3 -DNDEBUG -DSPECTRALFFT_FORCE_SCALAR=1 -Wall -Wextra -Wconversion -Wshadow -Isrc test/spectral_transition_test.cpp -o /tmp/soothe3-renderer-probe-scalar
/tmp/soothe3-renderer-probe-vdsp --benchmark-scaled
/tmp/soothe3-renderer-probe-scalar --benchmark-scaled
/tmp/soothe3-renderer-probe-vdsp
/tmp/soothe3-renderer-probe-scalar
```

The measurement session's complete phase output is retained locally in
`/tmp/soothe3-renderer-probe-vdsp.txt` and
`/tmp/soothe3-renderer-probe-scalar.txt`. No commit was made.
