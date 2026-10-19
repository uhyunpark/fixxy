# Soothe3 functional parity work

## Current naming checkpoint — 2026-10-20

Source naming now uses `FixxySound`, `fixxy_add_plugin`, `FixxyLookAndFeel`
and `fixxy` namespaces. AU factory prefixes become `Fixxy*AU`; bundle IDs,
manufacturer/plugin codes and the host parameter surface remain unchanged.
New preset writers use `.fixxyspectral` and `FixxySpectral` presetFormat; new
host-state writers use `FIXXY_SPECTRAL_STATE`. The old `.lpngspectral`,
`LpngSpectral` and `LPNGSPECTRAL_STATE` literals remain legacy reader inputs.
All nine product formats, 18 console suites and ReferencePluginHost now build
with zero compiler warnings/errors; the final workflow suite has 494 passing
assertions. All three staged VST3s pass pluginval strictness 5. AU identities and
new factory plist/export symbols match; VST3 class IDs and prepared parameter
surfaces/latencies remain compatible with Oct 19. Fresh installed AU registry
validation is not claimed: a staged-path host probe actually loaded the old
installed component, as confirmed by its dynamic-loader trace.
Soft/Hard staged renders match Oct 19 final baselines exactly (max error 0).
Seventeen real-main/key native snapshots passed in
`build/parity/fixxy-oct20-rename/`; root approved layout/theme/branding from the
fresh sidechain minimum/collapsed views. Their debug `max 207% OVER` observation
does not establish a real-time performance/deadline pass or DAW listening.
The FixxySound no-install hook kept bundle copies under
`build/fixxy-brand-staging`; all 27 installed bundle file hashes are unchanged.
Validation did not install or publish; subsequent publication is tracked in git history. Full evidence and limitations:
`fixxy-naming-verification-2026-10-20.md`.

## Historical integration checkpoint — 2026-10-19

Full soothe3 functional and sonic parity is **not complete**. This section
supersedes earlier handoffs for current source/build status; do not restart
completed research. The current increment passed its release gates and all six
branded AU/VST3 bundles are installed. This is not full soothe3 completion.

- Branding source now uses manufacturer `fixxy` and products `fixxy EQ`,
  `fixxy Comp`, `fixxy Spectral`. All nine format targets built; three branded
  VST3 pluginval strictness 5 runs passed. Root compared VST3 class IDs, AU
  codes/factory symbols and bundle IDs against the old installed bundles:
  all preserved. Spectral prepared parameter surfaces/latency also match.
  All six exact old bundles are preserved outside the scanner at
  `/Users/uhyun/Library/Application Support/fixxy/oct19-backup.RdSuzq`.
  Installation results belong to `soothe3-oct19-release-report.md`.
  Development build copying remains redirected
  by `build/fixxy-brand-noinstall.cmake` into `build/fixxy-brand-staging`.
- The complete new feature/runtime audits are
  `soothe3-feature-audit-2026-10-17.md` and
  `soothe3-runtime-audit-2026-10-17.md`. They explicitly include surround,
  preset browser/settings, gestures, phase/quality/latency and monitoring.
  Existing WOLA is not a minimum-phase or zero-latency implementation.
- External sidechain Stage 3 is complete: optional mono/stereo auxiliary bus,
  external detection, independent latency-aligned raw-key listening, physical
  meter and UI controls. `externalSidechain` and `sidechainListen` append to the
  old IDs; strict v3 migration validates active/inactive/clean snapshots.
  SC-off Soft/Hard and arbitrary chunks are bit-identical; six node fixtures
  differ by at most 7.45e-9 and the full processor route schedule by 1.21e-8.
  Prepared C++ heap instrumentation reports 0 allocations/deletions. See
  `soothe3-sidechain-port-report.md` and its Stage 2 report.
- Max-cut Stage 3 is complete with a shared per-hop 20 ms cap transition and
  post-ballistics stored/published/gain clamp. vDSP and scalar fixture errors
  are 3.10e-9; cap excess and chunk differences are zero, prepared heap 0/0.
  Stable Off preserves legacy cuts above 40 dB. This promises a per-frame
  spectral gain bound, not instantaneous waveform attenuation. See
  `soothe3-cap-port-report.md`; fixture inputs correctly match float audio.
- Independent 64-entry A/B undo journals remain. Fresh review found and fixed
  a listener-lock/metadata-lock inversion and UI drag/restore/close lifecycle
  defects. Journaling now uses explicit UI boundaries outside parameter
  dispatch. Deterministic RED-backed regressions and all 424 current-source
  workflow assertions pass, warning-clean; independent source review approved.
  See `soothe3-journal-lock-fix-report.md`. Do not restore the old listeners.
- All nine product targets are current with no source warnings; EQ/Comp
  required no relinking. All 18 console suites pass (the affected processor
  and workflow suites were rerun after the final meter-only change). Final
  staged Spectral VST3 pluginval strictness 5: SUCCESS. Logs use
  `build/validation/oct19-final-*`, with final UI gates under `post-meter-*`.
- Actual staged VST3 Soft/Hard renders match preserved legacy baselines
  sample-for-sample: maximum error 0 across 882,000 stereo float32 frames each
  at 44.1 kHz. This verifies SC-Off/MaxCut-Off preservation, not original soothe
  parity. See `oct19-final-preservation-comparison.log`.
- Installed AU validation passes for EQ, Comp and Spectral, identifying
  manufacturer/product as fixxy and resolving the exact new installed paths.
  The first EQ attempt hit a transient old registry entry; automatic registry
  refresh resolved it and the fresh retry passed. No cache deletion, registrar
  signals or Logic restart was needed. Installed bundle contents/signatures
  and all six executable hashes match the validated build staging.
- Root inspected minimum/collapsed real-main/key native harness captures in
  `build/parity/fixxy-oct19-final/`. Actual standalone inspection caught and
  corrected a disconnected-meter UTF-8 bug, now regression-tested. Native
  Hard/Undo restores the clean Soft baseline; theme/layout remain intact.
  `standalone-final.jpeg` records the final app. Its input remained safely
  muted: actual DAW listening and standalone live-input audio are not claimed.
- The scaled FIR probe still fails high-rate/automation deadline gates; some
  larger hops are unsupported. The alternative bounded IIR fitter also failed
  its unchanged 0.25 dB static gate (Gaussian maximum error 2.736 dB). Neither
  candidate is approved for production or phase/quality/latency UI. See
  `soothe3-renderer-probe-2026-10-17.md` and
  `soothe3-iir-fit-proto-report.md`; do not rerun rejected research by default.

After release gates: finish remaining audited matrix domains in separate gated
packages. The preset catalog is paused: its saved test is not integrated and
the helper is not implemented. True Detail/tilts/timing, summed stereo linking,
band focus/masks/listen, phase/quality/low-latency, browser/preferences and
surround are not delivered. Existing product latency remains 1024 samples.

Original trial access remains unavailable in the last known user state. The
19 historical original WAVs lack authoritative parameter/global settings and
cannot certify general sonic equivalence. A working legitimate reference or
fully documented comparison captures are still required for that final gate.
This checkpoint records pre-publication validation; subsequent publication is tracked in git history.

## Latest verified status — 2026-09-26

This checkpoint supersedes the historical interruption notes below.

- Soft/Hard selection is integrated in the existing N1024/H256 WOLA product,
  with processor automation, version 2 state migration and the existing color
  theme. Legacy Soft output is sample-identical to the pre-change VST3 render;
  latency remains 1024 samples. Main Detail affects Hard spectral width;
  legacy Detail tilt is explicitly Soft-only.
- Release AU/VST3/Standalone and all 16 console suites passed at handoff.
  Fresh product build, node/processor/workflow/schema tests, pluginval
  strictness 5 and auval passed on this date. Installed user AU/VST3 binaries
  match the build. Fresh logs are `build/validation/resume-*`.
- The earlier workflow timer failures were resolved by pumping the native
  event loop in the test. Missing-mode migration and malformed-state rejection
  checks now pass; neither issue is an outstanding release failure.
- Native signal snapshots retain the cream/ink/azure/ember theme and fit the
  minimum/collapsed layouts. Actual Logic listening remains a user check;
  expired reference access means original soothe3 sonic parity is unverified.
- FIR production integration remains paused after throughput failures. Full
  parity, including external sidechain/listen, new tilt/link behavior and
  phase/quality/low-latency modes, is not delivered by this Hard increment.
  No commits or pushes were made.

See `soothe3-hard-wola-port-report.md` for the numeric and audio-thread audit.

## Scope and evidence

Continue the existing mono/stereo fixxy Spectral product, preserving its cream,
ink, azure and ember palette. Target documented soothe3 behavior and measured
responses; proprietary algorithm identity and perceptual equivalence are not
established. Surround support is outside this mono/stereo pass. No commits.

The earlier stability/UI pass is recorded separately in `spectral-progress.md`.
Its passing validation does not validate the changes below.

## Work sequence and verification gates

1. Reference hosting: finish `test/reference_plugin_host.cpp`, inspect the
   installed soothe3 1.0.5 through public APIs, and capture parameter readback,
   latency and deterministic WAVs. A successful load alone does not prove an
   unrestricted license or a trustworthy audio measurement.
2. Research: finalize `soothe3-parity-dsp-spec.md`. Distinguish documented
   behavior, measured observations and candidate implementation formulas.
3. Python prototype: a separate agent verifies the chosen renderer against
   SciPy/direct convolution, then verifies detector, timing, focus, masks and
   control transitions. Export C++ fixtures only from passing calculations.
4. C++ DSP: another agent ports the verified prototype, with preallocated
   memory, bounded processing and explicit audio-thread audit. Compare output
   against generated fixtures and measure actual latency and CPU cost.
5. Workflow (parallel): processor-owned A/B, copy, undo/redo, parameter-only
   preset XML, factory reset, sidebar collapse and help. Verify state recall,
   host notifications and editor reopen behavior. Do not expose inert controls.
6. Integration: wire processor state, parameters, buses, monitors and UI to the
   tested engine. Keep schema/runtime checks synchronized. Verify legacy state
   loading, mono/stereo routing, variable blocks and parameter automation.
7. Product gate: warning-clean Release build, affected and full console tests,
   pluginval strictness 5, auval, and a visual signal check. Automated renders
   are not a listening verdict; explicitly report any unperformed DAW check.

## Historical checkpoints

- Reference host: built; our VST3 loads, exposes 59 parameters, reports 1024
  samples and renders a probe. soothe3 discovery succeeds but instantiation
  blocks during PACE module initialization (`recvfrom` in the sampled stack),
  while JUCE's event loop is serviced. No original parameter/latency/audio
  measurements are available from this host yet. Licensing status is unknown.
- Research: completed and reviewed. The finalized spec selects real-cepstral
  minimum-phase FIR, a direct 64-tap head and FFT tail; exact indexing and
  transition history reconstruction are specified. C++ remains gated on Python.
- Python: bounded static FIR, four-rate/mode, pure control, and revised
  phase-limited transition gates pass with scoped generated fixtures. The
  endpoint-only and queued five-segment policies remain in the rejection ledger.
  Full dynamic-engine integration remains unapproved pending C++ complete-path
  timing/audio-thread gates.
- Workflow: behavior tests written first; helper, UI and minimal processor
  persistence integration are owned by `workflow_parity`. Audio code unchanged.

## Integration decisions

- Ruling: use a causal direct FIR head with FFT-convolved tail as the preferred
  renderer candidate, conditional on numerical and CPU gates. It preserves more
  of an arbitrary spectral attenuation shape than a fixed small notch bank;
  if the gates fail, the architecture must change rather than relabel a delayed
  processor as zero latency.
- Ruling: workflow belongs to the processor, not the editor, so closing the UI
  does not erase A/B state. Preset snapshots contain parameters, not recursive
  workflow snapshots. Legacy bare `PARAMETERS` state remains readable.
- Ruling: leave the existing dirty worktree intact and coordinate file ownership
  between agents. Do not recreate or discard the previous completed pass.

## Historical verification findings

- Prototype found a counterexample to unrestricted two-bank FIR crossfading:
  valid 40 dB attenuation endpoints can blend to roughly 106 dB attenuation
  through phase cancellation. Endpoint bounds do not prove a transition bound.
  Transition design is pending numerical correction; do not port it as accepted.
- Reference loading is blocked in PACE initialization, not established as a
  JUCE event-loop deadlock. Do not bypass protection or infer license status.
  Asked the user whether original soothe3 opens normally in Logic; other work
  continues independently.

## Interruption checkpoint — 2026-09-09

All three implementation agents reached the account usage limit. Saved work is
incomplete and must not be treated as a release. Root ran fresh read-only gates:

- `cmake --build build --target SpectralWorkflowTest SpectralProcessorTest ReferencePluginHost -j 4`
  succeeded without emitted source warnings.
- `./build/SpectralProcessorTest`: all 6 checks passed (output, wet trim, Mix,
  aligned bypass and oversized-block cases).
- `./build/SpectralWorkflowTest`: 8 failures remain. Incomplete/nonfinite/
  out-of-range/unsupported-version presets are not rejected correctly and
  rejection is not atomic; modified-preset status is lost on host-state restore.
  A/B, copy, basic preset undo/redo, retained band bypass/delete, and basic
  editor/session persistence checks passed. Concurrency review remains open.
- `./proto/.venv/bin/python proto/spectral_parity.py`: partitioned convolution
  passed against SciPy/direct convolution (maximum error 1.67e-16), but synthesis
  stopped at the `max_root < 1.0` assertion. No full prototype PASS and no C++
  replacement-engine authorization. Remaining tests were not reached.
- `git diff --check` passed.
- Reference host still directly deletes an existing output before rendering.
  Required pending fix: reject same input/output and require explicit overwrite,
  preserving existing output until a successful render. Do not use it on user
  source audio or existing output files until this is fixed.

No fresh pluginval, auval, native signal visual check, or DAW listening gate has
been completed for this parity work. The previously installed plugin is not the
new replacement engine. Resume failing tests first, then continue the pipeline.

After the user's continuation, the three agents were resumed from these failing
checkpoints. Root's Computer Use connection successfully inspected Logic's
existing session; unavailable-audio-interface notices were dismissed, but no
project content, plugin setting or transport was changed. This was an app-state
check, not a signal/visual acceptance test or original soothe loading proof.

### Resumed numerical checkpoint

Root independently reran `test_renderer()` and `test_synthesis()` after the
prototype agent corrected the zero-polynomial coefficient order against SciPy.
Both passed: maximum convolution error 1.67e-16, dense magnitude error 0.012214
dB and small-design maximum zero radius 0.873314. This supersedes the earlier
synthesis assertion failure for the static 48 kHz suite only. Dynamic cap-safe
transitions and the complete engine remain unapproved. Primitive-only fixture
export was authorized to unblock a separate, non-integrated Stage 3 port.

### Reference host completion

`reference_host` completed its tool and report. Root independently verified the
saved bypass render: 882,000 stereo frames at 44.1 kHz, all finite, exactly equal
to the input. A direct same-input/output invocation was rejected with exit 1.
The earlier unsafe replacement finding is fixed: explicit `--overwrite` is
required and output is finalized in a sibling temporary file before replacement.
The original soothe3 PACE initialization blocker remains unresolved.

### Primitive port started

`fir_port` (separate Stage 3 agent) owns `SpectralFir.h`, its console tests,
additive CMake targets and hook. It consumes generated static fixtures only;
plugin integration and rejected dynamic transitions are explicitly out of scope
until their numerical gates pass.

## Product integration checklist

Passing a prototype or primitive test does not complete these product features.

| Capability | Current parity-work state |
| --- | --- |
| A/B, copy, presets, undo/redo | Wired; both independent-review regressions fixed and retested September 16 |
| Retained band bypass versus deletion | Helper and graph integrated; host automation presence tests green |
| Sidebar collapse and help | Wired; open-editor metadata restore and relayout regressions pass |
| Soft versus Hard detection | Python and C++ control primitives verified; Hard not available in the product |
| Monotonic Detail width and strength | C++ primitive verified; existing engine still uses earlier mapping |
| Six independent low/high Detail/timing tilts | C++ primitive verified; existing product still has three bipolar tilts |
| Minimum-phase and zero-buffer-delay FIR | Static C++ scalar/vDSP, fixtures and prepared heap checks pass |
| Cap-safe dynamic filter changes | Revised phase-limited Python gates pass; C++ transition port underway |
| Quality cadence and latency modes | Research/prototype only; not product features yet |
| External sidechain and sidechain listen | Not integrated |
| Band listen and per-band focus | Not integrated |
| Parallel bandpass masks | Python candidate; not integrated |
| Mix-aware complex reduction display | Not integrated |
| Full replacement-engine validation | Pending; old engine regression PASS is not this gate |
| Controlled original soothe comparison | Blocked on original plugin loading |

Resumed again on 2026-09-10 after another usage-limit interruption. Continue from
saved tests and files; do not restart the completed reference host or static
Python renderer work.

## 2026-09-11 verification checkpoint

Root ran `spectral_parity.run_all_gates(render=False)` after the reduced-grid
fixture revision. All seven scoped checks passed; the script correctly reports
PARTIAL PASS, not a complete dynamic engine. M=16384 is now the verified 48 kHz
design size (L=4096, P=64); M=8192 failed the unchanged narrow-notch error gate.
Worst static dense response error was 0.080918 dB; endpoint cap overshoot was
0.001151 dB. Four-rate tests scale up to M=65536/L=16384 at 192 kHz. Controls,
mode identity/null math and scoped fixture export passed. The existing five-step
transition is still not accepted for actual High/Ultra operation.

The static C++ port previously passed scalar/vDSP, heap and sanitizer checks.
Its reduced-grid rebuild and algebraically equivalent log-magnitude optimization
are in progress, with new benchmarks required. `fir_port` is separately acting
as Stage 1 researcher for a new phase-slew-limited transition policy; it must not
also implement that new policy. `parity_proto` remains the separate Stage 2
validator. Editor/graph workflow wiring is in progress after helper integrity
tests passed. No new DSP mode is yet connected to the shipping processor.

## Owners

- `design_check`: Stage 1 research/specification only.
- `reference_host`: reference host and its CMake target only.
- `workflow_parity`: workflow helper, editor/UI, tests and processor persistence.
- `parity_proto`: Stage 2 prototype and numerical fixture generation only.
- Root: coordination, independent review, gates and handoff.

## 2026-09-12 verification and 2026-09-16 resume

Root independently ran the revised `proto/spectral_transition.py`: all five
gates passed. The accepted admission uses `phiLimit - 0.01 rad`; the nominal
grid-only limit failed and is not accepted. Actual tested maximum phase was
0.184564380841 rad, maximum response error 0.058479426656 dB, and history/direct
error 5.90e-17. This is dense/refined numerical evidence, not a continuum
certificate. Fixed opposing shelves take 20 hops to settle; effective-cap lag
and moving-target lag remain explicitly documented in the prototype report.

Root independently built and ran `SpectralControlTest`: maximum error
1.88e-14, prepared heap allocation/deletion 0/0. Both static FIR backends passed
again. All 14 existing console executables passed, including workflow and
processor tests, with the two review gaps below not yet represented in that
green baseline. Current AU/VST3/Standalone builds succeeded; pluginval strictness
5 returned SUCCESS and `auval -v aufx Lpsp Uhyn` returned AU VALIDATION SUCCEEDED.
These validate the existing audio engine plus initial workflow increment, not
the replacement DSP or subsequent review fixes. Build copying updated the
user-local legacy `LpngSpectral` AU/VST3 bundles. Standalone's invalid post-link signature
was replaced with an ad-hoc signature and strict verification passed.

Root visually inspected native signal snapshots at the minimum size, collapsed
sidebar and contextual-help states: layout is readable and the existing palette
is preserved. Actual desktop inspection could not start because Computer Use's
native pipe failed twice; no new live DAW/Standalone signal or listening verdict.

Independent workflow review found two Important issues, now assigned back for
test-first correction:

1. Undoing a preset or factory load restores sound but not preset identity and
   its clean/dirty baseline; metadata must be part of the undo transaction.
2. Workflow-only host restore must notify an already-open editor even when
   APVTS values are unchanged; sidebar state changes must also rerun layout.

The account usage limit interrupted both implementation agents before these
fixes and the transition port were completed. Resumed on September 16:
`workflow_review` owns those two fixes; `control_port` owns the separate Stage 3
transition port, complete-path benchmarks and audio-thread audit. No transition
C++ code existed at the interruption. `parity_proto` now provides a read-only
product integration gap/ordering review. No commits and no new DSP mode has
been connected to the production processor yet.

### First audible product increment decision

After the read-only integration review, root selected an additive Hard detector
on the existing N1024/H256 WOLA path as the first audible increment. This does
not replace or relabel the unfinished FIR engine. Legacy Soft, existing public
parameter ranges/IDs, dry alignment and 1024-sample host latency remain unchanged.
The planned `detectionMode` choice defaults to Soft; old presets and host/A/B
snapshots must migrate missing values to Soft atomically, preserving identity,
presence and clean/dirty state. No new mode selector ships before its audio path.

`parity_proto` now owns a separate Stage 2 integration gate for the actual
sqrt-Hann window, absolute calibration, final-audio Hard level/depth sweeps,
off-bin behavior and mode transitions. A separate implementation agent will
port only a passing handoff. The FIR transition port and complete-path timing
continue independently. New-model timing/tilts/linking/masks are not silently
substituted into legacy sessions, and exact soothe matching remains unverified.

### September 16 workflow acceptance

The two Important workflow findings are fixed. Root reviewed the scoped preset
identity undo action and non-audio revision/relayout code, then independently
reran workflow and processor console tests: all pass. New assertions cover
preset names, parameter and presence-only clean baselines, factory undo/redo,
and workflow-only restore into an open editor with unchanged APVTS values.
Root inspected fresh signal snapshots for both collapsed and expanded host
restores under `scratchpad/workflow-ui-september16`.

Post-fix AU/VST3/Standalone build succeeded; pluginval strictness 5 returned
SUCCESS (`build/validation/workflow-reviewed-pluginval-console.log`), and AU
validation succeeded (`build/validation/workflow-reviewed-auval.log`). Standalone
was re-signed ad hoc after linking and strict signature verification passed.
Live desktop verification remains unavailable because the Computer Use native
pipe still fails to start. These gates do not validate unfinished Hard/FIR work.

### September 22 resume

Another usage-limit interruption stopped the remaining DSP agents. Root resumed
their existing files, then freshly built and ran both `SpectralTransitionTest`
and `SpectralTransitionScalarTest`: PASS. Maximum combined errors were 2.49e-14
and 4.27e-14, with prepared allocation/deletion counts 0/0. Repeated admissions
at H64/128/256 agree with the direct oracle; new-bank history, all 64 impulse
offsets, and admitted effective-cap tests pass. High-rate/scalar and every-hop
parameter-automation performance gates remain with `control_port`.

The new `spectral_hard_wola.py` is not yet a passing integration handoff. Root's
fresh run passed actual-window calibration, then failed legacy-Soft equivalence
at 0.1572696413 maximum error. `parity_proto` is investigating the framing/
reference mismatch; no Hard production port is authorized until its gate passes.

Root subsequently reran the corrected Hard WOLA suite: all four scoped gates
PASS. The old batch reference's documented extra-N slice was replaced by the
unchanged legacy detector plus an independently framed SciPy STFT/ISTFT oracle;
no tolerance was relaxed. Soft error is 8.33e-17, SciPy audio error 1.11e-16,
switching error 2.78e-16, chunk error 0, and tested depth/level monotonicity holds.
The C++ port must also reproduce the new Soft fixture before changing the node,
and keep all historical C++ Soft fixtures passing. Off-bin bin-amplitude bias
reaches -2.10 dB in this window; no perfect peak calibration is claimed.

`control_port` now owns the separate additive Hard node port. `workflow_review`
owns additive mode schema/state migration and subsequent processor/UI wiring.
Existing IDs/ranges/automation remain unchanged; version 2 state/presets allow
strict validation while migrating version 1's missing mode to Soft. Legacy
tiltDetail sensitivity is explicitly Soft-only and will be disabled/explained in
Hard; the main Detail knob continues to affect the legacy spectral width.

The FIR transition port is numerically complete but rejected for production
throughput. Coordinated tests include every-hop automation of Detail, timing
and all six tilts: 48 kHz H64 exceeds its mean deadline, H128 has sampled
deadline misses, and every tested 96/192 kHz cadence fails mean throughput.
See `soothe3-transition-port-report.md` for all percentiles and exclusions.
No quality/rate fallback, dropped-update policy or pretend zero-latency mode was
introduced. FIR optimization/support policy remains a separate future gate.

### Reference access clarification and interruption

The user confirmed that the original soothe3 free trial has expired and the
plugin cannot currently be opened for comparison. Further original-host load
attempts are not useful without renewed legitimate access. Public behavior,
independent numeric references and our own regression tests remain sufficient
to continue implementation; controlled original sonic/response equivalence is
explicitly unverified.

All three agents then hit the account usage limit again. The Hard WOLA final
report and generated fixtures are saved, but read-only inspection confirms no
`detectionMode` or `setDetectionMode` production code has been added yet. State
migration/UI wiring and the additive Hard C++ port remain pending, not partially
released features. Resume `control_port` for the bounded node port and
`workflow_review` for the coordinated schema/state/processor/UI assignment;
do not restart the passed Python or FIR primitive work. No commits.

Root's final interrupted-work verification: build and diff check pass; node,
processor, control, both static FIR and both transition executables pass. The
new unchanged-node Soft fixture test passes at 7.38e-9 audio error. However,
`SpectralWorkflowTest` now exits 1: the newly added missing-`detectionMode`
assertion is expected RED, and five existing collapsed-editor refresh/layout
assertions fail on two runs. The latter previously passed and must be
investigated, not ignored. The test sleeps 60 ms then calls pending timers
without pumping the native event loop, so timer startup/delivery is a lead,
not an established diagnosis. This current suite is not green; no new plugin
release or completion claim is warranted.
