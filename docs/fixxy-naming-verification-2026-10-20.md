# fixxy naming verification — 2026-10-20

Source branding now uses `FixxySound`, `fixxy_add_plugin`, `FixxyLookAndFeel` /
`FixxyLookAndFeelLight` and namespace `fixxy`. The shared header is
`src/plugins/FixxyLookAndFeel.h`. `README.md` contains the project overview and
build/test commands; `docs/architecture.md` preserves the former `PROJECT.md`
architecture/history; root and directory instructions now use `AGENTS.md`.

New presets use `.fixxyspectral` and `FixxySpectral`; new host sessions use
`FIXXY_SPECTRAL_STATE`. The reader still accepts `.lpngspectral`, `LpngSpectral`
and `LPNGSPECTRAL_STATE`. These are centralized in `SpectralStateFormat.h`.
The open dialog accepts new presets, legacy presets and XML. Source/test old-name
matches are confined to these compatibility constants and migration/rejection
fixtures; the paused, untracked preset-catalog test was preserved untouched.

## Build and console gates

Release was configured with the old `CMAKE_PROJECT_LpngSound_INCLUDE` unset and
`CMAKE_PROJECT_FixxySound_INCLUDE` pointing at
`build/fixxy-brand-noinstall.cmake`. Its AU/VST3 copy destinations are exclusively
`build/fixxy-brand-staging`. All nine EQ/Comp/Spectral AU, VST3 and Standalone
targets, all 18 console targets and `ReferencePluginHost` built with `-j2`.
A final incremental build caught documentation-comment timestamp changes.
Both build logs contain zero compiler warnings/errors.

All 18 suites passed: BandResponseTest, MatchedKernelTest, CompressorNodeTest,
DynamicEqTest, ParamSchemaTest, CompUiMapTest, SpectralNodeTest, SpectralUiMapTest,
AnalyzerEngineTest, SpectralProcessorTest, SpectralWorkflowTest,
SpectralControlTest, SpectralFirTest, SpectralFirScalarTest,
SpectralTransitionTest, SpectralTransitionScalarTest, SpectralCapNodeTest and
SpectralCapScalarTest. Fresh workflow count: **494 passing assertions**, including
old-brand v1/v2/v3 migration, active/inactive and clean snapshots, preset identity,
dirty state, bypassed band presence and atomic malformed/wrong-brand rejection.

Logs: `build/validation/oct20-rename-{configure,build,current-build}.log`,
`oct20-rename-console-summary.log` and `oct20-rename-<TestName>.log`.

## Plugin and compatibility gates

All three staged VST3 bundles passed pluginval strictness 5: EQ, Comp and Spectral
each ended `SUCCESS`; see `oct20-rename-pluginval-{EQ,Comp,Spectral}.log`.

AU type `aufx`, manufacturer `Uhyn`, bundle IDs `com.uhyun.{EQ,Comp,Spectral}`
and subtypes `Lpeq`, `Lpcm`, `Lpsp` match the installed Oct 19 bundles.
New AU plist factory names match the actual exported symbols:

| Product | Plist factory | Exported symbol |
| --- | --- | --- |
| EQ | `FixxyEQAUFactory` | `_FixxyEQAUFactory` |
| Comp | `FixxyCompAUFactory` | `_FixxyCompAUFactory` |
| Spectral | `FixxySpectralAUFactory` | `_FixxySpectralAUFactory` |

Both VST3 class IDs per product match Oct 19, as do their bundle IDs:

| Product | Audio class CID | Controller CID |
| --- | --- | --- |
| EQ | `ABCDEF019182FAEB5568796E4C706571` | `ABCDEF011234ABCD5568796E4C706571` |
| Comp | `ABCDEF019182FAEB5568796E4C70636D` | `ABCDEF011234ABCD5568796E4C70636D` |
| Spectral | `ABCDEF019182FAEB5568796E4C707370` | `ABCDEF011234ABCD5568796E4C707370` |

Fresh VST3 host inspection matches the installed Oct 19 parameter metadata,
bus counts and prepared latency exactly at 44.1/48 kHz: EQ 178 parameters / zero
latency, Comp 14 / zero, Spectral 62 / 1024 samples. See
`oct20-rename-identities.log` and `oct20-rename-host-compatibility.log`.

**No fresh installed AU registry/runtime validation is claimed.** A direct-path
Spectral AU host probe was given the staged bundle, but `DYLD_PRINT_LIBRARIES`
proved JUCE loaded the installed Oct 19 component. Its successful inspection
therefore does not validate the new source. Evidence is in
`oct20-rename-inspect-AU-Spectral.stderr`; no `auval` result was substituted.

The processor `.cpp` files have no diff. DSP math, parameter values, palettes,
geometry and audio-thread processing remain unchanged. The state/preset changes
affect serialization branding and accepted reader tags; version 3 is retained.
Existing callback heap checks still pass with zero allocation/deletion, and no
new callback locks, I/O, logging or unbounded work were introduced.

## Signal and preservation checks

`SpectralProcessorTest --snapshots build/parity/fixxy-oct20-rename` passed and
saved 17 native captures with real main/key signal, including minimum/collapsed
Soft/Hard views, sidechain meter views and restored host metadata. Key files are
`spectral-892x520.png`, `spectral-details-collapsed.png`,
`spectral-hard-{minimum,collapsed}.png` and
`spectral-sidechain-{minimum,collapsed}.png`. The saved readback preset uses
`.fixxyspectral`. Root inspected the fresh sidechain minimum/collapsed captures
and approved layout, theme and branding: the signal/key meter is readable, with
the same colors/layout and no overlap. The debug readout showed `max 207% OVER`
in the native harness; this callback timing observation is not a validated
real-time deadline/performance pass. No timing tuning or display hiding was
performed. These captures do not establish live-input listening in a DAW or
standalone app.

Staged final Soft and Hard renders used `build/parity/final-control-render.wav`,
`--trim-latency`, and Hard `--set 'Detection Mode=1'`. The new
`build/parity/oct20-rename-{soft,hard}.wav` files each match their Oct 19 final
baseline sample-for-sample: **maximum absolute error 0** over 882,000 stereo
float32 frames at 44.1 kHz. See `oct20-rename-render-{soft,hard}.log` and
`oct20-rename-sound-comparison.log`. This verifies rename preservation, not
original soothe3 parity.

All 27 files inside the six installed fixxy AU/VST3 bundles retain identical
SHA-256 hashes before/after validation; see
`oct20-rename-installed-{before,after}.sha`. No user plugin installation,
commit, push or remote mutation was performed.
