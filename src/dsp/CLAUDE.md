# src/dsp/CLAUDE.md

Shared node-library contract. Every node here is a candidate node in the eventual
graph library — the conventions below exist so nodes stay swappable and safe to
build off the audio thread.

## Lifecycle

Uniform interface, no exceptions:

- `prepare(sampleRate, maxBlockSize, numChannels)` — off the audio thread. Does all
  allocation (pre-sized buffers, per-channel state) and coefficient setup.
- `reset()` — clears state (filter memory, envelope followers, etc). Safe to call
  from a background thread before an ENABLE fade-in (see MultibandEq.h bypass rules)
  so a node never resumes from stale memory.
- `process(...)` — audio thread. Allocates nothing.

## Parameters as data

Nodes take a `Params` struct, not hardcoded members — the future LLM layer emits
these as JSON (see `ParamSchema.h` for the machine-readable descriptor layer that
will carry this to the LLM: id, type, range, unit, scale hint). Per-effect schema
tables (`CompressorSchema.h`, `EqSchema.h`) live next to their node headers.

## State and precision

- Per-channel state is pre-allocated in `prepare()`, flat arrays/`std::array`, no
  `std::vector` growth in the hot path.
- State and coefficients are **double** precision throughout, even though audio
  buffers are float. Coefficient recompute and envelope math accumulate error
  fast enough at float precision to matter.
- JUCE-free, header-only. No atomics here — atomics for cross-thread publishing
  (meters, GR display) belong in the plugin layer (`src/plugins/CLAUDE.md`), not
  in a node.
- Constructible and preparable off the audio thread — this is what makes the
  lock-free graph-swap architecture possible (PROJECT.md 난제 1).

## Audio-thread absolutes in `process()`

No allocation, no `delete`, no mutex, no I/O, no logging, no exceptions, bounded
work per call. Coefficient recompute (the `exp()`/`tan()`/matched-filter algebra)
happens in the params setter, **outside** the per-sample loop — once per block at
most, never per sample.

## Realization note: matched types run Direct Form I

Bell / LowShelf / HighShelf / Tilt / Notch / Bandpass (`SvfBandNode.h`) use the
Ivantsov matched-IIR coefficients (`MatchedCoeffs.h`) realized in **Direct Form I**
— state is literal past input/output samples, not an eigenbasis. This replaced an
earlier normal-form realization once the matched path started modulating
coefficients at sub-block cadence (dynamic EQ); a normal form's state needs
reinterpretation whenever coefficients change and is ill-conditioned near a
repeated pole. Full derivation and the proto measurement that forced the switch:
see `SvfBandNode.h`'s header comment — do not re-derive it here, cite it.
Lowpass/Highpass (all slopes) stay on the TPT-SVF Butterworth cascade
(`BandResponse.h`) — a matched LP cascade accumulates too much Nyquist residual
at high slopes.

## Every kernel change needs a verified ground truth

A DSP change to this directory ports from a **verified** `proto/*.py` module, never
re-derives in C++. Each ported kernel gets a console test (`test/*_test.cpp`) whose
fixtures are dumped from the proto by a `dump_*.py` script (`proto/CLAUDE.md`), not
copied by hand from proto printouts.

## UI-facing math shares a single source

Where a value drives both the audio kernel and a drawn UI curve, the recipe lives
in exactly one header, consumed by both sides — see the `BandResponse.h` /
`MatchedCoeffs.h` pattern: `computeSectionRecipe()` / `matched::computeBiquad()` are
called by both `SvfBandNode::computeSection()` (audio thread) and the UI's response
evaluator. A curve that says one thing while the filter does another is worse than
no curve (PROJECT.md 함정: 이음새 버그).
