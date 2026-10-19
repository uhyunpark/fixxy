# Existing WOLA Max-cut: verified Stage 2 handoff

2026-10-17. Bounded bug fix only; independent fixxy automation policy, not
recovered manufacturer behavior. No product files changed; no commits.

## Port equations (verbatim)

```
H = 256; tauCap = 0.020 seconds; snapTolerance = 1e-6 dB
legacyBound = max(40, 16.167635300821875 * (10/5) * 4)
            = 129.341082406575 dB
off = requestedMaxCut >= 24
targetCap = off ? legacyBound : requestedMaxCut

fresh effectiveCap = targetCap
fresh finalClampBypass = off

once per actual internal hop, BEFORE any lane gain calculation:
    aCap = exp(-H / (sampleRate * tauCap))
    effectiveCap = aCap*effectiveCap + (1-aCap)*targetCap
    if abs(effectiveCap-targetCap) <= snapTolerance:
        effectiveCap = targetCap
    finalClampBypass = off && effectiveCap == legacyBound

per active lane/bin, AFTER existing target cap and frequency smoothing:
    s1 = max(target, aR*(s1-target) + target)
    y = aA*(previousY-s1) + s1
    previousY = finalClampBypass ? y : min(y, effectiveCap)
    publishedReduction = previousY
    gain = pow(10, -publishedReduction/20)
```

`target` in the last block is the existing smoothed reduction demand, not
`targetCap`. Preserve its existing pre-ballistics cap for enabled requests.
Preserve `s1` unchanged by the final clamp, and store the clamped `previousY`.
Updating only the meter/gain while keeping an unrestricted `previousY` fails
the history contract. Inactive cores keep their existing unity/no-state-change
behavior; the shared cap still advances once per hop.

The recurrence changes neither H256/N1024 framing nor existing 1024-sample
latency. Off must bypass all **final** clamps exactly once snapped; retaining
an unconditional 40 dB clamp would change legitimate legacy Soft processing.
Later enable ramps from the current effective value. The residual threshold
and exact snap are explicit finite-completion policy, for enabled and Off
targets; do not wait for floating-point asymptotic equality.

## Prototype API and fixture bridge

`proto/spectral_cap_wola.py` extends existing `HardWola` with local wrappers;
existing target computers and reconstruction are reused unchanged.
`Cap(fs, requested).step(requested)` advances the shared state.
`CapWola(..., cap_events={inputSampleIndex: requestedDb})` adds automation;
`render(..., chunks=...)` returns delayed audio and recorded processor state.
An event applies before accepting the indexed sample. A hop completed exactly
at that index uses the previous request; a non-hop event applies at the next
completed hop. This event convention is explicit so a C++ harness can apply
setters at the same point, independent of host callback grouping.

`scalar_step` uses Python scalar operations independently of the imported
optimized ballistics/control implementation. Every active bin's stored output
and release history is checked against it. SciPy STFT/ISTFT independently
reconstructs audio using the recorded gain decisions; this checks transform,
framing, M/S routing and overlap-add, not independent detector design.

Run `./proto/.venv/bin/python proto/dump_spectral_cap_fixtures.py`. It first
runs all prototype gates, then regenerates `test/spectral_cap_fixtures.h`:

- 112 six-field scalar rows, including deep initial history, zero, rise/fall,
  finite Off completion, gain reflection and re-enable.
- Analytic deterministic stereo input recipe, exact parameter/event tables,
  selected audio probes and `[frame][lane][bin]` reduction probes for Soft/Hard
  in L/R and M/S, plus corresponding effective caps. Arrays use doubles.

Fixture carrier samples use `source().astype(np.float32).astype(float)` before
the Python reference render: evaluate each analytic sine sum in double, cast
the completed sample to float, then promote to double for reference DSP. This
matches the plugin/test float input domain; double fixture storage does not
imply double input. On October 18, the C++ port exposed a **1.63e-6** audio
fixture difference when the exporter instead supplied unquantized double
carriers. Float quantization changes tiny spectral components and therefore
the relative Soft detector as well as reconstruction. The exporter now uses
the same input samples; no algorithm or acceptance tolerance was changed.
The general prototype gates below continue to use their original double
analytic sources; the exported C++ probes use the float carrier domain.

## Gates and measured evidence

Command above passed. Independent scalar error **0 dB**, retained-release
error **0 dB**, alternate-chunk audio difference **0**, and SciPy maximum
audio error **1.249000902703301e-16**. Stable Off matches the legacy renderer
bit-for-bit for both modes and both stereo representations. Valid extreme
Soft settings reached **50.83111481249679 dB**, proving 40 dB is insufficient.

Scalar history tests cover 44.1/48/96/192 kHz, 1000+ ms release-equivalent
poles, falling/rising/zero requests, startup, finite Off snap and re-enable.
Integrated WOLA tests cover 48 kHz, both detectors, both lanes, L/R and M/S,
non-hop events at samples 8193/11001/13003/14501, both odd and batch chunks,
zero/enabled mono startup, and exact stable-Off arithmetic. For each active
frame/bin, assert `0 <= reduction <= effectiveCap` (numerical tolerance
1e-10 dB after log round-trip) and `gain >= 10^(-effectiveCap/20)` (1e-12).
The independent hop timeline also asserts the cap advances once, not once
per lane or callback.

Automation diagnostics: largest consecutive output-sample difference across
the tone test **0.1736745872030526**; Hann-windowed automation-minus-stable
spectral residual norm outside ±4 FFT bins around all input carriers
**0.0037881740847105854** (FFT norm divided by record length). These are
reproducible diagnostics, not a click/audibility threshold or listening pass.
No WAV suite was generated; representative listening remains a separate gate.

Only a **per-frame spectral gain bound** is established. Previously processed
frames remain in the overlap-add buffer; waveform interference also prevents
turning that bound into an instantaneous sample-domain gain/attenuation bound.
Any effective-cap display must describe the same reduction frame. No final
audio-limit claim is authorized by these tests.

Stage 3 still requires C++ fixture comparisons, warning-clean build, relevant
console tests, pluginval strictness 5 SUCCESS and explicit audio-thread audit
(no allocation/deletion/locks/I/O/logging; bounded work). UI changes additionally
require the existing real-signal standalone/DAW visual gate. No such C++ or
native-host gate is claimed by this Python report.
