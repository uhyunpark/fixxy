# Additive Hard WOLA port — 2026-09-24

Stage 3 ports the independently verified `proto/spectral_hard_wola.py`; this is
not the FIR transition design and makes no original-plugin sonic-parity claim.
The original trial has expired. Legacy Soft remains the default, with its
arithmetic, N1024/H256 WOLA, retained envelopes and 1024-sample latency unchanged.

## Numeric and integration gates

- New Soft baseline was tested before the port: maximum fixture error
  `7.37951078023e-9`; Hard `7.42516342855e-9`; retained-state switching
  `7.37951078023e-9`. All old Soft fixtures and unchanged tolerances pass.
- Fourteen independent prototype comparisons covering mono, stereo, M/S,
  focus, link, weight and timing tilts: maximum error below `7.46e-9`.
  Irregular-block versus one-shot output is exact.
- Literal DFT plus independently evaluated Hard knee and legacy spatial
  averaging checks actual DC/interior/Nyquist calibration at Detail 0 and 10:
  maximum published float-reduction error `8.22067871553e-7 dB`.
  Main Detail changes Hard width; Detail strength tilt has exactly no effect.
- Input-level/depth monotonicity, fresh silence, release to silence, global
  40 dB limit and existing falling-MaxCut envelope semantics pass.
- Depth zero equals unchanged Soft output exactly. Delayed-input residual is
  at most `5.24076225125e-17`, below the unchanged `1e-9` null gate. An initially
  overstrict new bit-exact input assertion confused FFT roundoff with a defect;
  no existing gate or production DSP was changed to address it.
- Fresh `SpectralNodeTest`, `SpectralWorkflowTest`, `SpectralProcessorTest` and
  `ParamSchemaTest` pass. Scalar FFT and vDSP node suites both pass. Node
  AddressSanitizer/UndefinedBehaviorSanitizer run passes without diagnostics.
  Focused target rebuild and direct scalar/sanitizer builds are warning-clean.
- Cached processor mode forwarding changes actual audio, preserves the earlier
  Soft prefix during live switching, and recalls Hard state with exact audio.
- Copied-tree unversioned/v1 migration inserts missing Soft mode into active,
  A/B and clean snapshots; legacy parameters/identity/dirty state survive.
  Version 2 writes retain Hard; malformed current active/inactive/clean mode
  data rejects before live mutation. Existing workflow regression tests pass.
- Real-signal native snapshots and UI assertions pass in
  `build/parity/hard-integration/`: `spectral-hard.png`,
  `spectral-hard-minimum.png`, `spectral-hard-collapsed.png`, and
  `spectral-hard-restored.png`. Default/minimum/collapsed images were inspected:
  mode pill fits, only Detail tilt is disabled, main Detail stays active.
  Host Soft changes and Hard restores refresh enablement correctly.

## Audio-thread audit

The added processor operation is one cached relaxed atomic load plus an integer
setter; no parameter lookup, state migration or UI work occurs there. The node
adds a fixed-size signal scan and at most two 513-bin Hard target loops per
existing hop. It reuses prepared buffers, FFTs, spatial smoothing and ballistics;
no new FFT, history reset or render branch is introduced. Analysis-window
normalization is computed during prepare. Mode selection affects the next
completed hop's target; existing envelopes smooth the resulting gain.

Added prepared operations contain no allocation, delete, mutex, I/O or logging.
Instrumented ordinary C++ new/delete counts are `0/0` across prepared setters,
mode switches and rendering. This counter does not intercept internal C-library
allocation; the unchanged FFT backend remains prepared off-thread. Work is
bounded by fixed bin/frame sizes and the supplied block length.

## Owner release gates

Owner verification completed: Release AU/VST3/Standalone build and all 16
console suites passed. Pre-change versus post-change Soft renders are sample
identical; Hard changes the output (maximum difference 0.0430937856), with
finite samples and unchanged 1024-sample latency. Existing parameter IDs,
defaults, ranges and own-parameter indices are preserved.

On 2026-09-26, the product build and node/processor/workflow/schema suites
were rerun successfully. Fresh pluginval strictness 5 reported SUCCESS and
auval reported AU VALIDATION SUCCEEDED. Logs: `build/validation/resume-*`.
Installed user AU/VST3 executables match the built executables byte for byte.

The bounded callback diagnostic is not a real-time guarantee: one 96 kHz /
256-sample scheduler outlier exceeded the block deadline. Native real-signal
snapshots were inspected, but actual Logic listening and original soothe3 A/B
remain unperformed. No FIR production integration or commit was made.
