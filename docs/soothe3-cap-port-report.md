# Bounded WOLA MaxCut C++ port

2026-10-18. Ports only the verified `spectral_cap_wola.py` policy in
`soothe3-cap-proto-report.md`; no commit and no renderer/phase redesign.

## Implementation

`SpectralNode` advances one shared cap before either lane on each actual H256
hop, including inactive hops. The 20 ms coefficient is prepared off-thread.
Requested Off (>=24 dB) targets exactly 129.341082406575 dB; residual <=1e-6 dB
snaps to the requested target. Stable Off bypasses the final clamp exactly.
Fresh preparation/reset (and setters before the first hop) seed the request.

Existing pre-target caps, detectors, frequency smoothing, and release `s1`
remain unchanged. After attack/release, stored `y1`, published lane reduction,
and gain all use the same clamped value. Existing inactive-core behavior and
all external-sidechain paths remain intact. Framing and latency remain N1024.
No new public test API: the isolated console test exposes private state only
after pre-including dependencies, allowing actual lane histories to be checked.

## Numeric and regression gates

The new test failed against the pre-port node: audio mismatch
0.0151465930368, stored-reduction mismatch 35.1344603901 dB, cap exceedance
42.2500393624 dB. Chunk identity and prepared heap checks already passed.

An initial post-port comparison exposed a fixture input-domain mismatch:
Python used double audio while the public C++ input is float. The Stage 2 agent
corrected the exporter to render float32-quantized input promoted to double and
reran its independent gates. No DSP formula or tolerance was changed.

Warning-clean Release CMake build of `SpectralCapNodeTest`,
`SpectralCapScalarTest`, and `SpectralNodeTest` succeeded with
`-Wall -Wextra -Wconversion -Wshadow`. Both cap targets pass:

| Gate | vDSP | Scalar FFT |
| --- | ---: | ---: |
| Audio vs generated proto (gate 1e-7) | 3.09853619151e-9 | 3.09853619151e-9 |
| Stored lane reduction (gate 1e-5 dB) | 6.55031584529e-14 | 9.42579347907e-14 |
| 112 scalar cap/y/s/gain/bypass rows (gate 1e-10) | 9.94759830064e-14 | 9.94759830064e-14 |
| Frame cap excess / shared cap timeline error | 0 | 0 |
| Arbitrary/oversized callback difference | 0 | 0 |
| Prepared new/delete count | 0/0 | 0/0 |

Coverage includes Soft/Hard, L/R and M/S, both lanes, deep retained history,
falling/rising/zero/Off automation at non-hop sample boundaries, gain reflection,
stable Off Soft reduction above 40 dB, and finite Off/enabled completion.
44.1/48/96/192 kHz tests cover inactive-hop progression, Off re-enable,
reset/reprepare, and initial requested cap. Active mono zero-cap startup is
checked in both detector modes. Callback sizes 1/37/137/256/479/4096 include
calls larger than the prepared maximum 97.

The complete existing `SpectralNodeTest` passes. External-sidechain fixture
maximum audio error remains 7.44605399561e-9; disabled-sidechain output is
bit-exact legacy output for both modes. Existing Soft, Hard, sidebar, framing,
latency, and M/S routing tests retain their previous gates.

## Audio-thread audit

- Allocation/deletion: no containers or buffers added to processing. Three
  scalar state members; preparation owns coefficient evaluation. Instrumented
  prepared setters, processing, resets, and direct gain recurrence: 0/0.
- Locks/atomics: none added; node remains JUCE-free and atomic-free.
- I/O/logging: none in node processing.
- Bounded work: constant-time cap advance once per completed hop; one branch
  and minimum per active bin within the existing fixed 513-bin, <=2-lane loop.
  No new loop or retry on the processing path. `exp` runs in preparation only.
- Release history: `s1` is not clamped; `y1` stores the clamped result, as the
  numeric history fixture independently checks.

This establishes a per-frame spectral gain bound only. Retained overlap-add
and waveform interference prevent any instantaneous sample attenuation promise.
The policy is this project's explicit 20 ms/snap design, not a recovered
manufacturer automation law.

Full combined product build, pluginval strictness 5, and native-host visual
validation belong to the parent integration gate after all source packages
finish. This report does not claim those checks for the post-cap product binary.
