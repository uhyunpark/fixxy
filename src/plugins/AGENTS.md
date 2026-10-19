# src/plugins/AGENTS.md

Plugin-layer conventions — one independent JUCE product per effect
(`src/plugins/eq`, `src/plugins/comp`, ...), sharing the `src/dsp/` node library.

## APVTS parameter naming

Real APVTS parameter ids are generated through a single helper — e.g.
`bandID(i, suffix)` in `src/plugins/eq/PluginProcessor.cpp` — so `"band3Freq"` etc.
is produced in exactly one place and can't drift between the parameter layout and
the code that reads it. Parameters are versioned via `juce::ParameterID{id, ver}`.

## Schema layer must stay drift-guarded

`src/dsp/ParamSchema.h` + the per-effect tables (`CompressorSchema.h`, `EqSchema.h`)
are the machine-readable description of the plugin's real parameters that the
future LLM layer will consume — they must never silently diverge from the actual
APVTS layout. Two-part guard:

- `test/param_schema_test.cpp` (`ParamSchemaTest`, plain C++, no JUCE) checks the
  schema tables against an independently transcribed copy of the APVTS layout —
  catches drift against *intent*.
- A `JUCE_DEBUG`-only runtime check in each plugin's constructor
  (`PluginProcessor.cpp`) walks the same schema tables against the *live* `apvts`
  object — catches drift against *reality*.

Both must be updated together whenever a parameter is added, renamed, or
re-ranged.

## Atomics live here, not in dsp/

Cross-thread publishing — meter values, gain-reduction display, the sample rate
the editor's curve view reads — uses `std::atomic` members on the processor (e.g.
`publishedSampleRate` in `PluginProcessor.h`). `src/dsp/` nodes stay atomic-free;
the plugin layer owns the thread-safety surface between the audio thread and the
message thread.

## UI single-source rule

Any curve or meter drawn in the UI is evaluated from the exact same coefficients
the audio kernel runs — see the `BandResponse.h` / `MatchedCoeffs.h` shared-recipe
pattern (`src/dsp/AGENTS.md`). Never re-derive filter math in UI code.

## The analyzer lesson

Spectrum dB axis and EQ gain axis are **different physical quantities** — mapping
spectrum levels (-40..-90 dBFS bins) onto an EQ gain window (±30 dB) clamps
everything to the floor and produces a blank analyzer that passes every unit test.
Keep them on separate scales (see `AnalyzerView.h`'s dedicated analyzer dB window
vs. the EQ gain window). Full incident writeup: docs/architecture.md 함정 (이음새 버그).

## Verification for any plugin change

- Console tests pass (whichever of `BandResponseTest` / `MatchedKernelTest` /
  `CompressorNodeTest` / `DynamicEqTest` / `ParamSchemaTest` / `AnalyzerEngineTest`
  the change touches).
- Zero-warning build (`-Wall -Wextra -Wconversion -Wshadow`).
- `pluginval` strictness 5 SUCCESS, `auval` SUCCEEDED (macOS AU validation).
- UI-facing changes additionally get a visual check with real signal in the
  standalone app or a DAW — component-level tests passing does not prove the
  seam between components works (see the analyzer lesson above). Component unit
  tests alone missed exactly this class of bug.
