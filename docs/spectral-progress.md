# fixxy Spectral progress — September 8, 2026

This pass completes the current spectral processor's correctness and native JUCE
editing work. It preserves the calibrated transfer constants and the existing
cream, ink, azure, and ember theme. It does not establish sonic equivalence to
soothe3.

## Completed audio fixes

- M/S mode changes no longer reinterpret unfinished overlap-add history. Each
  inverse frame is decoded to L/R before entering the OLA buffer, so the history
  remains in one canonical basis while L/R and M/S are switched.
- `SpectralNode` divides calls larger than its prepared maximum block size into
  bounded chunks. `SpectralProcessor` does the same for its preallocated dry
  scratch and wet path, covering ordinary processing and host bypass.
- The continuous output-stage controls `mix`, `output`, and `wetTrim` advance
  their 20 ms ramps once per sample instead of stepping once per host block.
- `processBlockBypassed()` uses the latency-preserving processing path. Spectral
  history stays warm and the returned dry signal remains aligned to the
  processor's reported latency.
- `ReductionPublisher` uses atomic payload elements as well as its sequence
  counter. Accepted reads contain one complete generation without a C++ data
  race; rejected reads preserve the previous destination. Writer work is fixed
  and reader retry count remains bounded.

See [SpectralNode.h](../src/dsp/SpectralNode.h),
[PluginProcessor.cpp](../src/plugins/spectral/PluginProcessor.cpp), and
[ReductionPublisher.h](../src/plugins/spectral/ReductionPublisher.h).

## Completed interface fixes

- The selected-band card is a foreground child, so the opaque analyzer cannot
  paint over its panel or badge. It uses two rows without overlap at the minimum
  892 × 520 size and one row at 1132 × 640 and above.
- The plot and gestures cover the real 20 Hz–20 kHz parameter range. Default
  range pins no longer jump inward on first drag, and 20 Hz and 50 Hz have grid
  labels alongside the existing reference frequencies.
- The asynchronous band menu uses a `SafePointer` lifetime guard before applying
  a selection, so closing the editor while the menu is open cannot dereference
  its destroyed band layer.
- Tooltips cover processing controls and band interactions. The selected band's
  `Gain` label now reads `depth`, clarifying that it changes suppression
  weighting rather than applying EQ gain.
- The spectral editor's macro hierarchy is 144 px depth, 88 px detail, and
  52 px attack/release at normal sizes, with proportional reduction at the
  minimum height. Other plugin editors are unchanged.
- Sidebar labels show their complete names (`release`, `max cut`, `wet trim`),
  and bipolar zero values are normalized so the UI displays `0.00`, not `-0.00`.
See [SpectralEditor.h](../src/plugins/spectral/ui/SpectralEditor.h),
[SpectralAnalyzerView.h](../src/plugins/spectral/ui/SpectralAnalyzerView.h),
[SpectralDisplayMap.h](../src/plugins/spectral/ui/SpectralDisplayMap.h), and
[WeightBandLayer.h](../src/plugins/spectral/ui/WeightBandLayer.h).

## Sound and feature boundaries

The kernel is soft-style only and uses a fixed 1024-sample FFT. It reports 1024
samples of latency when prepared: approximately 23.2 ms at 44.1 kHz (21.3 ms at
48 kHz). There is no hard mode, quality selector, or low-latency mode.

Implemented controls are depth/detail/timing, eight weighting bands, range
limits, L/R or M/S operation, link/focus, frequency tilts, max cut, wet trim,
mix/output, bypass, and delta. `tiltDetail` is only a frequency-dependent depth
bias; it does not vary notch width. Attack/release are millisecond parameters,
and the tilt controls are single bipolar values rather than separate low/high
controls.

External sidechain, band listen, presets, A/B comparison, undo/redo, collapsible
sidebar sections, and separate channel-reduction displays are not implemented.
APVTS host-state serialization exists, but it is not a preset browser. Disabled
bands disappear from the plot and their slots can be reused; there is no separate
retained-but-bypassed band state.

The [official soothe3 manual](https://oeksound.com/manuals/soothe3/) remains a
future behavior reference, not evidence about this implementation's sonic
accuracy.

## Verification

Fresh September 7 validation:

- Release builds of `SpectralProcessorTest`, AU, VST3, and Standalone exited 0
  with no compiler warning or error diagnostics. See
  [final-build.log](../build/validation/final-build.log).
- All ten console suites passed: `BandResponseTest`, `MatchedKernelTest`,
  `CompressorNodeTest`, `DynamicEqTest`, `CompUiMapTest`, `ParamSchemaTest`,
  `AnalyzerEngineTest`, `SpectralNodeTest`, `SpectralUiMapTest`, and
  `SpectralProcessorTest`. See
  [final-console-tests.log](../build/validation/final-console-tests.log).
- M/S transition identity error was at most 1.234 × 10⁻¹⁷ over the tested
  64/127/256/1000-sample chunks. Active mono/stereo transitions stayed finite;
  mono ignored M/S exactly.
- Node calls prepared for 128 samples and processed as 512, 3001, or one-shot
  calls matched the bounded reference exactly at depth 0 and depth 5. The
  processor's 8193-sample ordinary and host-bypass paths also matched their
  128-sample references exactly.
- Output, wet-trim, and mix ramp maximum errors were 2.272 × 10⁻⁸,
  2.216 × 10⁻⁸, and 1.744 × 10⁻⁸ respectively, all below the 3 × 10⁻⁸ gate.
  Host bypass reproduced the 1024-sample delayed impulse with zero error.
- The publisher's fresh 25,000-generation concurrency test accepted only
  coherent frames, preserved the destination on rejected reads, and returned
  the final generation after the writer stopped.
- The 20 Hz–20 kHz coordinate tests passed, including endpoint drag
  preservation, with maximum relative frequency round-trip error 1.103 × 10⁻⁶.
- `pluginval` strictness 5 exited 0 with `SUCCESS`; `auval -v aufx Lpsp Uhyn`
  exited 0 with `AU VALIDATION SUCCEEDED`. See
  [final-pluginval.log](../build/validation/final-pluginval.log) and
  [final-auval.log](../build/validation/final-auval.log).
- On September 8, the local Standalone bundle received an ad-hoc signature and
  passed strict `codesign` verification. This is not a Developer ID signature or
  notarization. See
  [final-standalone-signature.log](../build/validation/final-standalone-signature.log).
- The native JUCE snapshot harness selected a band successfully, fed synthesized
  stereo tones plus noise through `processBlock()`, and wrote all five captures.
  Root visual review approved the populated analyzer, unclipped controls and
  labels, compact selected-band layout, delta-only view, and clean unselected
  state: [892 × 520](../build/validation/spectral-892x520.png),
  [1132 × 640](../build/validation/spectral-1132x640.png),
  [1600 × 900](../build/validation/spectral-1600x900.png),
  [delta](../build/validation/spectral-delta.png), and
  [unselected](../build/validation/spectral-unselected.png). Harness results are
  in [final-snapshots.log](../build/validation/final-snapshots.log).

Earlier scoped sanitizer checks passed for the node under ASan/UBSan and for the
25,000-generation publisher test under TSan. Those sanitizer runs were not
repeated in the September 7 final validation above.

The visual evidence is from the native JUCE harness, not a browser. Standalone
audio-device playback, Logic/DAW listening, and representative musical-material
listening remain unverified; no listening-based or sonic-equivalence claim is
made here.
