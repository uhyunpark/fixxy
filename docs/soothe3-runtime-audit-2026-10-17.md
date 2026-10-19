# soothe3 runtime and implementation audit — 2026-10-17

Read-only source audit; only this report was added. No product source changes,
builds, plugin installation, commits, or original-plugin loading attempts were
made. Existing dirty worktree and concurrent fixxy rename work were preserved.
References below use repository-relative paths and inspected line numbers;
concurrent edits can move those numbers.

## Decision

Continue the tested N1024/H256 WOLA product for independently shippable
sidechain, band, control and workflow increments. Keep the minimum-phase FIR
replacement outside production until a separately budgeted complete-path
architecture gate passes. A small FFT/convolution primitive benchmark is not
that gate. Reusing its numeric primitives does not require adopting its renderer.

The current product has substantive verified DSP, but it is not full functional
or sonic parity. Reference trial access has expired; original algorithm identity,
controlled response matching and perceptual equivalence remain unverified.

## Evidence inspected and fresh diagnostics

Read `AGENTS.md`, DSP/plugin/test conventions, `PROJECT.md`, the latest checkpoint
in `soothe3-parity-progress.md`, control/FIR/transition/Hard port reports, and the
production node, processor, schema, publisher, UI and workflow source.

The existing September-built executables were run without rebuilding:

- `./build/SpectralControlTest`: PASS, maximum error `1.87627691162e-14`, prepared
  heap allocation/deletion `0/0`.
- `./build/SpectralNodeTest`: ALL PASS. Includes legacy/Hard fixtures, depth-zero
  identity, exact chunk invariance, M/S transitions, oversized callbacks, Hard
  level/depth response, calibration and the existing falling-cap behavior.

These confirm cached executable behavior, not compilation of concurrently edited
source or new product validation. No fresh pluginval, auval, UI signal inspection,
performance measurement or DAW listening was performed for this report.

The [official manual](https://oeksound.com/manuals/soothe3/), inspected October 17,
states separate low/high tilts, channel-specific curves, parallel bandpasses,
mix-aware reduction, external key monitoring, independent A/B histories, and
multichannel operation. Quality changes temporal resolution; minimum phase is
the default, linear phase adds delay, and low latency constrains the other modes.
It also describes global preferences and preset browsing. These are behavioral
requirements, not public implementation formulas. The manual does not prove our
chosen detector, linking or filter math matches the original.

## Reusable pieces and boundaries

| Piece | Reuse directly | Boundary that must remain explicit |
| --- | --- | --- |
| `SpectralNode.h:69`, `:132`, `:328` | Prepared N1024/H256 framing; latency 1024; oversized-block chunking; existing mono/stereo and retained-history tests | Fixed two-channel delayed WOLA; neither causal minimum-phase rendering nor zero-delay rendering |
| `SpectralNode.h:577` | Hard detector with actual sqrt-Hann amplitude calibration and independently generated audio fixtures | Uses legacy MAX linking, width/timing, weights and focus; does not integrate the complete new control model |
| `SpectralControl.h:19`, `:38` | Six independent low/high controls and frequency-dependent timing/Detail coefficients | Standalone candidate model; normalized attack/release differ from shipping millisecond parameters |
| `SpectralControl.h:94`, `:107`, `:128`, `:141` | Follower/output clamp, cap smoothing, bandpass-mask union, exact range masks | Caller must order and integrate these; helper coverage does not validate WOLA audio or UI seams |
| `SpectralControl.h:163` | Prepared exact median/box smoothing and width refresh | Replacing the legacy resampled median changes sound and timing cost; requires a new model/version and recalibration |
| `SpectralFir.h:19`, `:102`, `:141` | Verified static synthesis, fixed-head convolution, coefficient/history reconstruction | Static correctness does not establish dynamic production throughput; no linear-phase implementation exists here |
| `SpectralTransition.h:49`, `:92` | Verified phase-limited admission, history-correct two-bank transition, explicit effective cap | Average-throughput failures remain; effective cap intentionally lags requested cap; dense numerical evidence is not a continuum proof |
| `ReductionPublisher.h:24` | Fixed storage, lock-free atomic payload, bounded writer and bounded UI retry | Current payload is one scalar curve, not per-channel complex response or a coherent full rendering snapshot |
| `SpectralMix.h:45`, processor `:419` | Tested aligned dry/mix/wet trim/delta/bypass algebra and prepared dry scratch | New monitoring/modes need an explicit precedence policy and matching latency |
| `SpectralWorkflow.h:149`, `:478` | Strict parameter-only XML, versioned copied-tree migration, A/B sound/presence/identity baselines, processor ownership | One UndoManager; switching slots clears history; current migration knows only versions 1/2 and missing detectionMode |

## Priority findings

### 1. MaxCut is presently a target cap, not a final moving limit

Both detector branches cap `redRaw` before frequency smoothing at
`SpectralNode.h:588` and `:600`. The follower emits its old release/attack state
unclamped at `:628–633`. Consequently, reducing MaxCut does not immediately
bound even the newly published bin reduction. This is deliberate legacy behavior
and is asserted by `test/spectral_node_test.cpp:691`; the fresh cached node test
passes that assertion. It conflicts with a proposed final-cap promise, so changing
the test silently would erase a compatibility requirement rather than fix it.

The candidate `Ballistics::clamp()` (`SpectralControl.h:101`) addresses the new
control output, retaining release memory. `test/spectral_control_test.cpp:176`
checks that distinction. Integrating that clamp alone still cannot justify an
instantaneous final-audio claim: WOLA overlap at `SpectralNode.h:528–541` contains
older frames rendered under older caps. Parameter smoothing and frame/output
delay must be reflected in a specified effective cap and display timestamp.

Define the contract before a new renderer: requested knob, smoothed cap,
admitted/rendered effective cap, global design ceiling, and measured final
response are distinct quantities. A time-varying signal does not have one exact
static frequency response, and a spectral MaxCut is not an instantaneous
sample-amplitude ratio. Verify frozen sinusoidal transfer, dense off-bin probes,
and falling-cap transitions separately. Do not substitute a bin assertion for
final audio tests or claim FIR's lagged `effectiveCap()` meets the current knob.

Hard has a 40 dB target ceiling through `hardReduction()`. Legacy Soft is not
universally clamped to 40: `maxDbArr` can exceed it with legal depth and Detail
tilt settings (`SpectralNode.h:270–280`), and MaxCut 24 means OFF. This is an
intentional legacy contract, not evidence that original soothe is bounded the
same way. New model maxima must not silently rewrite old sessions.

### 2. Adding a sidechain bus alone would break fixed channel bounds

The processor currently exposes only main input/output (`PluginProcessor.cpp:77`)
and accepts mono/stereo (`:318`). It derives `numChannels` from the whole process
buffer (`:346`) then indexes `wet[2]` and `dryRing[2]` (`:421–425`, header `:151`).
A sidechain input can make that buffer include extra input channels. The first
sidechain package must isolate JUCE main-bus and sidechain-bus views; auxiliary
channels must not enter the main-output loops or output clearing policy.

The node derives detection exclusively from program spectra (`SpectralNode.h:433`,
`:446`, `:471`); a second independent, synchronized analysis history is required
for an external key. Reusing the program spectrum or replacing the input waveform
would process the wrong signal. Missing/disabled key handling must be explicit;
the accepted candidate control contract uses zero demand for an enabled but
missing key, never automatic fallback (`soothe3-control-port-report.md:151`).

### 3. The displayed reduction cannot describe actual channels or mix

Dual-core processing discards the smaller channel reduction at
`SpectralNode.h:464–465`. The processor copies one last-hop curve before any
description of mix/trim is published (`PluginProcessor.cpp:454–456`). The editor
draws that same curve (`SpectralAnalyzerView.h:222`, `:301`) regardless of Mix or
wet trim. Thus Mix=0 can play aligned dry while displaying nonzero reduction.
This is a demonstrated source-level semantic mismatch, not merely missing polish.

Publish both channel results from the actual render, including basis, effective
cap, epoch/hop time, and matching blend state in one coherent snapshot. For a
frozen L/R filter, normalized-to-dry output is
`(1-m) + m * wetGain * Hwet(f)`; attenuation comes from its complex magnitude,
not an interpolation of reduction dB. Output gain may be excluded from the
reduction axis if specified consistently. For minimum phase the phase term
matters, especially at partial Mix. For WOLA, a frozen diagonal gain plot is
only a control-map view until verified against its actual signal response.

In unlinked M/S, L/R output is a 2x2 transfer matrix: encoding/decoding combines
different M/S filters. It cannot be represented as two independent scalar L/R
responses without defining the displayed basis or signal-dependent measurement.
The report/UI must name which quantity is shown rather than label a target map
as an exact measured response.

Processor host bypass is also invisible to `SpectralAnalyzerView`: audio uses
`hostBypassed || bypassParam` (`PluginProcessor.cpp:351`), while the view reads
only `bypassParam` (`SpectralAnalyzerView.h:209`). A host invoking the bypassed
callback can therefore play transparent dry while the view still shows cuts.
Include effective render/monitor mode in the same published snapshot.

### 4. Three bipolar legacy tilts cannot represent six independent shelves

Shipping parameters are `tiltDetail`, `tiltAttack`, `tiltRelease`, each -1..1
(`PluginProcessor.cpp:212`, `SpectralSchema.h:179`). The node applies opposing
frequency tilts about 1 kHz; legacy Detail tilt changes strength only and is
ignored in Hard (`SpectralNode.h:581`; editor `:895`).

The candidate six-control implementation already exists in
`SpectralControl.h:19–55`, including independent width/detector/timing changes.
It is not a new UI attachment to the three existing values. It requires per-bin
width intervals and detector memory; legacy `buildOctaveWindows` creates one
uniform width, and the production Soft path has no separate detector follower.
Do not reinterpret legacy IDs or change their ranges/skews to normalized values.

### 5. A/B sound snapshots exist; slot histories do not survive navigation

`SpectralWorkflow::selectSlot()` calls `applySnapshot(..., false)` at `:104`,
which clears the sole UndoManager at `:589` and `:625`. Tooltips describing active
slot undo do not supply independent histories. Existing A/B/preset preservation
tests do not establish this missing behavior.

APVTS uses a constructor-bound UndoManager (`PluginProcessor.cpp:81`). A robust
small replacement is a bounded workflow edit journal of complete parameter,
presence and preset-identity snapshots per slot, with gesture boundaries and
explicit suppression during undo/restore. If retaining granular APVTS undo,
implement a deliberate slot-aware action router; merely adding another
UndoManager member cannot redirect existing attachment-generated actions.
Decide host-automation history semantics explicitly and test them. This work is
off the audio thread; the processor must continue reading cached atomics only.

## Minimum packages with independent acceptance gates

### A. External key input and sidechain listen on the existing renderer

Scope: safe main/aux bus separation, optional mono/stereo key, key enable, key
meter, detector-source selection and key monitoring; retain the current WOLA
wet path and 1024-sample host delay. Add cached parameter pointers and publish
effective source/monitor state; do not query APVTS or construct buffers in audio.

Node API: program buffers and optional detector buffers are separate. Prepare
key FIFO/rings/spectra off-thread and advance on the same hop clock. Apply the
same defined L/R-to-M/S transform to a stereo key; choose and test mono-key mapping
and mismatched bus layouts. Feed program FFTs to synthesis and key FFTs only to
detection. SC-off must retain the legacy path/sample identity. Use the actual
analysis-window normalization for Hard. No generic detector-engine rewrite is
needed for this package.

SC listen needs its own output route and latency alignment, distinct from delta.
Specify priority with bypass, band listen and delta, whether output trim applies,
and whether the analyzer displays key or program. Preserve key samples before
any in-place output writes. Add only implemented controls.

Gate: Python prototype for routing/detector behavior; generated fixtures;
independent processor tests where main-only signal cannot trigger enabled silent
SC, key-only signal triggers program suppression, key changes never leak directly
to program output, listen reproduces the defined delayed key, SC-off matches the
legacy fixture, mono/stereo/M/S and oversized/irregular blocks are safe. Prepared
heap audit, warning-clean build, pluginval with SC bus layouts, auval and a real
signal UI check are required. Measure the extra analysis and second detector
cost in small callbacks before release.

### B. Per-band focus, mask union and band monitoring

Scope: append per-band focus and explicit slope where applicable; retain all
existing type ordinals. `WeightBand` currently has only on/type/frequency/gain/Q
(`SpectralWeight.h:39`), and both DSP and UI simply sum dB responses
(`SpectralNode.h:649`, `WeightBandLayer.h:580`). Two bandpasses therefore multiply
magnitudes rather than form a parallel pass region; distant bands can suppress
one another. This is an algorithmic difference.

Create one shared curve/mask evaluator that returns separate per-channel
sensitivity and eligibility masks. Reuse `BandWeightEval` for band shape and the
verified `BandpassMask` primitive for union policy. Apply masks after sensitivity
and frequency smoothing, with a deliberately tested temporal/mask policy.
Use the same evaluator for two UI depth curves. Keep retained bypass/presence
semantics: disabling a node removes its contribution, while deletion frees it.
Do not use presence metadata as an audio-thread lock-taking query.

Band listen is an output monitor, not the band's contribution to sensitivity.
Use a prepared audition filter/mask with smooth monitor switching, delta/solo
preference and optional on-drag activation; closing the editor or ending a drag
must not leave an accidental temporary solo. A new audition filter is new DSP:
derive/prototype it independently rather than invent it in the UI layer.

Gate: disjoint bandpasses pass the union; permutation/order invariance; bypass,
delete, overlap and zero-focus identity; per-band focus in L/R and M/S; serialized
focus/slope and A/B clean state; UI/audio shared evaluator assertions; audible
monitor chunk/null tests and real-signal visual inspection. This can use WOLA
without waiting for phase modes.

### C. New control-model version on WOLA

Scope: append the six new tilt controls; expose normalized timing only through
new compatible parameters or an explicit model/version. Reuse control primitives
and test at the actual N1024/sqrt-Hann analysis calibration. Avoid simultaneously
changing the renderer and envelope estimator: first independently verify the
new coefficients/detector/follower with a clear A/B against legacy.

Smooth every continuous control and update coefficients on a sample-count/hop
schedule that is independent of host callback size. Existing `skip(numSamples)`
is evaluated once before the whole callback (`PluginProcessor.cpp:374–377`), so
automated detector controls can take different sequences for different host
block partitions. Existing chunk tests hold controls fixed; they do not establish
automation invariance. Sidebar values are explicitly unsmoothed (`:379–392`).

Gate: default new-model fixtures, six independent shelf/locality checks,
Detail width/strength monotonicity, timing step traces, both detectors and link
mapping, block-partition-invariant automation, prepared cost with all controls
changing. Global control and final-render cap tests are separate; do not claim
full final MaxCut from the helper alone.

### D. Per-channel/mix-aware graph publication

Scope: replace the one-curve payload with a fixed-capacity coherent render
snapshot; retain bounded no-retry audio publishing. Include both active lanes,
basis, effective mode/cap/blend and frame identity. Read-side curve building is
message-thread work; GUI parameter reads must not be mixed with old DSP frames.

Gate: asymmetric inputs/focus produce separate curves; Mix=0 shows neutral
normal output; depth=0/wetTrim=0 null; wet trim and phase affect partial Mix as
the complex formula predicts; internal and host bypass agree with actual output;
delta/listen axes are explicitly defined. Validate transfer against rendered
steady probes, then inspect real signal. No second full audio FFT is needed for
an honest control-map view, but an exact signal-response claim needs its oracle.

### E. Real quality, phase and latency configurations — separate feasibility gate

The production constants are compile-time (`SpectralNode.h:69–72`), host latency
is reported only during preparation (`PluginProcessor.cpp:275`), and dry rings
are exactly N long (`:300`). There are no live quality/phase/low-latency APIs or
linear-phase FIR generator. Shrinking N or reporting zero delay does not create
a minimum-phase causal processor. Adding delay to WOLA does not prove linear
phase either: its realized filterbank response must be measured.

Specify one capability matrix with real renderer, rate, cadence, phase, lookahead,
maximum delay and measured support. Prepare all required memory/FFT plans off
thread. Switch complete prepared configurations at a defined boundary, retaining
or deliberately rewarming history and fading aligned outputs. Notify host latency
from an appropriate non-audio workflow; define behavior when hosts cannot change
PDC seamlessly. Preallocate maximum dry/monitor alignment. Quality locks and
non-realtime Ultra override select actual supported configurations, not labels.

The rejected FIR path's complete automated stereo measurement is decisive:
48 kHz/H64 mean `2230.442 us` exceeds `1333.333 us`; H128 sampled max `2796.625 us`
exceeds `2666.667 us`; every tested 96/192 kHz cadence fails mean throughput.
Even the relatively successful 48 kHz/H256 run excludes host/UI, SC, band edits,
monitoring and transitions. Source: `soothe3-transition-port-report.md`.

Performance-safe candidates to investigate, not validated replacements:

1. Keep WOLA for the additive features above. Parameterize its prepare-time
   geometry only if a separate resolution/cadence/CPU test justifies it.
2. Rework FIR update cost before integration: shared transforms/history only
   when responses truly coincide, partition scheduling/cache layout and measured
   exact coefficient/window optimizations. Retain numeric response/transition
   gates. Smaller design grids/tap counts have already failed narrow-notch
   accuracy; do not repeat that search without a changed specification.
3. A worker can remove synthesis bursts from a callback only with sufficient
   sustainable throughput, bounded queues, latest-target policy, late-result
   behavior, explicit detector lag, immutable ownership and off-thread retirement.
   The current history reconstruction/refresh operations also need placement and
   measurement. Moving the current overloaded average to a worker is not a fix.
4. A smaller constrained dynamic filter family could reduce cost, but would
   require new research/prototype/response/phase/transition gates and an explicit
   approximation tradeoff. It is not a drop-in parity claim.

First acceptance here is one complete two-channel configuration, including
automation, program/key analysis, actual rendering, history changes, mix/dry
alignment and UI publication at the supported maximum rate and minimum callback.
Measure mean/p99/max against callback deadlines; heap-free fixed work alone is
insufficient. Only then expose its real modes or extrapolate to more channels.

### F. Per-slot undo, preset browser and global settings

Split three independent off-audio packages:

- Slot undo journal: preserve separate A/B edit/redo histories through navigation;
  undo parameters, band presence and preset identity atomically; define copy
  behavior and bounded capacity. Test edit A, edit B, return A, undo/redo A, then
  verify B and its history remain unchanged; test host restore/editor reopen.
- Preset browser: current menu is only baseline/open/save-as
  (`SpectralEditor.h:503`); reuse strict load/save and identity helpers behind a
  file index with category/search/favorite/user-default/previous-next/preview.
  Disk enumeration and metadata persistence remain off audio. Ship original
  fixxy presets; original factory preset content/sound equivalence is not verified.
- Global preferences: persist outside host preset snapshots using an off-thread
  settings owner, distribute effective settings to instances as immutable/atomic
  data, and keep quality/low-latency locks in the appropriate instance state.
  Tooltips/listen/cursor behavior can precede DSP quality overrides. Do not expose
  offline-Ultra/global-low-latency promises until package E implements the modes.

### G. Surround is a new topology, not a channel-limit edit

Current hard limits appear in node arrays (`SpectralNode.h:72`, `:950`), processor
dry storage/pointers, `SpectrumFifo.h:34`/`:88`, pairwise M/S code and one-curve
publication. Increasing `kMaxChannels` alone leaves two-core math and unsafe
processor indexing. Prepare a layout-derived channel/set/pair map, per-channel
history/control/render states, pair-local M/S, channel-set depth and linking,
per-band assignments, bypass/solo/delta routing and selected-channel metering.

Linking must operate in a specified detector domain and order, with pair/group/
all interactions tested numerically. Current program MAX linking differs from
the candidate RMS-power link (`SpectralControl.h:112`); neither becomes verified
original behavior just by naming it sum. Preserve mono/stereo legacy routing as
a separate compatibility case. Test speaker identities and impulses per channel,
LFE policy, layout rejection, set isolation, link combinations, state restore,
monitor precedence and each supported maximum layout's complete CPU/memory cost.

## Parameter/state compatibility requirements

1. Keep selected `fixxy` display branding while preserving bundle identity,
   manufacturer/plugin codes, APVTS IDs, state tree names, `LpngSpectral`
   presetFormat and `.lpngspectral` compatibility as recorded in `PROJECT.md`.
2. Append IDs; preserve existing choice ordinals and current host parameter
   ordering/normalized ranges. Existing attack/release are milliseconds, MaxCut
   24 is OFF, and three legacy tilt IDs retain their old meaning. A new global
   model selector/version must migrate old sessions to their old audio behavior.
3. Update the real APVTS layout, cached pointer reads, schema table, independent
   schema test and runtime drift guard together. Wire a control only after its
   tested audio behavior exists. No inert quality/phase/listen labels.
4. Extend strict state and preset versions. Current `readLegacyVersion()` accepts
   only 1/2 and `migrateDetectionMode()` adds only one known field
   (`SpectralWorkflow.h:470–490`); validation requires the exact child count
   (`:519`). Merely appending parameters will reject older snapshots. Migrate
   active, inactive A/B and clean baselines on a copied tree before any live
   mutation; reject malformed new-version states atomically. Preserve clean/dirty
   identity and retained-band presence through migration.
5. Separate transient audition state, per-instance recallable options and global
   preferences. Define which operations participate in host automation, preset
   loads, A/B copies and undo, rather than storing everything in APVTS by default.

## Smallest next independently verifiable package

For an immediate package independent of DSP, implement package F's real per-slot
undo histories first. Keep the existing workflow public navigation/query methods
(`selectSlot`, `copyActiveToOther`, `canUndo`, `canRedo`, `undo`, `redo`) so editor
call sites stay small. Add an explicit message-thread UI-edit boundary, for example
`beginUiEdit(name)` / `endUiEdit()`, and a scoped suppression mechanism for replay,
slot recall and host restore. Use bounded history per slot. Each transaction must
record its affected parameter values plus presence/preset-identity changes; avoid
replaying an entire old parameter snapshot over unrelated later host automation.
Merge overlapping gestures for one node drag into one transaction. Parameter
changes still notify the host through the existing normalized setter.

Exact plumbing to replace/bridge: processor UndoManager/APVTS binding at
`PluginProcessor.h:57–61` / `PluginProcessor.cpp:81`; workflow references at
`SpectralWorkflow.h:27–29`, `:124–145`, `:246`, `:259`, `:580–643`, `:689`;
slider/combobox attachments in the editor and node gesture begin/end at
`WeightBandLayer.h:218–254`, plus one-shot gesture helpers at `:472–492` and
`SpectralEditor.h:908`. The current workflow parameter listener (`:449`) only
observes band enabling with an atomic bit mask; it is not a general edit journal.
No history allocation or metadata locking may enter a parameter callback that can
run on the audio thread. Existing granular JUCE undo actions must not execute as
a second competing source of history after the journal takes ownership.

Required first failing test: edit A, switch to B, edit B, return to A, undo/redo A,
then switch to B and undo/redo B; both histories and sounds must remain distinct.
Also test redo-tail truncation confined to the edited slot, no-op switches, one
node-drag transaction, preset/factory sound plus identity and presence undo,
defined destination-history behavior for A-to-B copy, editor close/reopen,
host restore clearing both transient histories, no history recursion during
replay, and unrelated host automation surviving UI undo. History need not be
serialized; host parameter/state IDs and audio processing stay unchanged.

The next audio capability is package A's bus-safe detector-source split,
external-key enable and SC listen on WOLA. Its SC-off path can be held to exact
legacy audio identity without adopting the rejected FIR renderer. Keep band
audition and the six-tilt model in later packages to isolate their oracles.

If selecting a correctness repair before feature work, a separate versioned
cap-control experiment is smaller: add a failing falling-cap output-bin test,
prototype post-ballistics clamping/effective-cap timing, port it behind the new
model, and test actual WOLA output across overlapping frames. Its completion
criterion is precisely the proved cap domain, not full final-audio/soothe parity.

Before any renderer replacement, save authoritative **our-product** regression
fixtures with exact parameter/state payloads, source WAV hashes, sample rate,
layout, host chunk sequence and reported/measured delay. Require default/legacy
Soft sample identity where promised; Hard fixtures; dry/depth-zero/delta nulls;
mono/stereo/M/S routing; all existing automation IDs/ranges/indices; old bare,
v1 and v2 active/A/B/clean snapshot migration; presence/identity/dirty recall;
oversized/irregular blocks; and current trim/mix/bypass algebra. New-model audio
gets independently generated fixtures rather than a forced identity assertion.
Old reference bounces without authoritative settings are not a sonic oracle and
must not be used to claim equivalence to the inaccessible original.

Every audio package still requires the research → independent NumPy/SciPy gate
→ separate C++ port, an explicit allocation/delete/locks/I/O/logging/bounded-work
audit, warning-clean builds, relevant/full console tests, pluginval strictness 5,
auval and real-signal UI inspection when applicable. No commit is authorized.
