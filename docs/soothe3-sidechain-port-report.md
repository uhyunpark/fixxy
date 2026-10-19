# External sidechain and independent listen — Stage 3

Date: 2026-10-18. Ports the verified equations and schedules in
`docs/soothe3-sidechain-proto-report.md`; no formula was re-derived. This is our
external-detector/monitor behavior on the existing N=1024/H=256 WOLA, not a
claim of proprietary soothe3 implementation or sonic parity. The original
reference product remains unavailable.

## Implementation

- `SpectralNode` retains its three-argument API and adds defaulted physical-key
  pointers/count plus `setExternalSidechainEnabled()`. Separate prepared key
  FIFOs, physical L/R history and FFT scratch stay warm while SC is disabled.
  The count attached to the final sample controls each completed hop.
- Mono key broadcasts to both detector lanes, including M/S. Stereo key into
  mono detection uses RMS magnitudes; stereo M/S uses complex sum/difference.
  Existing MAX linking, focused depth, Soft/Hard recipes and main carrier remain.
- Missing/silent external lanes generate zero NEW demand before the Soft knee,
  retaining the established release state. SC-off output is bit-identical in
  both detector modes. The future post-ballistics cap seam was not changed here.
- Processor bus views distinguish mono/stereo main input/output from optional
  absent/mono/stereo auxiliary input. Physical key is copied before host-buffer
  writes; fixed two-channel wet/dry arrays never use total main+aux channels.
- Separate prepared key delay aligns independent raw listen to N=1024. Monitor
  priority is Bypass > SC listen > Delta > Normal, with verified 5 ms route-weight
  ramps, including interrupted fades and bypass. Existing gain smoothing stays.
- Appended `externalSidechain` and `sidechainListen` booleans default false;
  older IDs, ranges and order remain. Schema checks independently assert the
  new ordering/defaults; the existing live APVTS drift guard consumes that schema.
  Physical key peak publishes through `sidechainLevelDb` (floor -100 dB).
- The separate workflow/UI owner implemented strict v3 migration and toolbar
  controls, including older v1/v2 active/inactive/clean snapshots, preserved v1
  detection-mode migration, and atomic rejection. Its workflow suite passed.

## Numeric and product gates

All old and new `SpectralNodeTest`, `SpectralProcessorTest`, `ParamSchemaTest`
and `SpectralUiMapTest` assertions passed. No existing numeric tolerance or
prototype fixture was changed. Measured maxima:

| Check | Maximum absolute error |
| --- | ---: |
| Six SC audio fixtures: Soft/Hard/mono-M/S/RMS mono/missing/source+mode switches | 7.44605399561e-9 |
| Independent scalar 513-bin Soft/Hard frame-4 gain, via published float dB | 1.56765760373e-8 |
| Missing-key delayed identity | 1.38777878078e-16 |
| SC-off Soft/Hard versus legacy node calls | 0, bit-identical |
| Chunk sizes 1/37/256/479/4096, prepared maximum 97 | 0, bit-identical |
| Mid-hop connection switches at 333/777/1051 under arbitrary chunks | 0, bit-identical |
| Disconnection zero-target release recurrence, published float dB | 9.24961771176e-7 dB |
| Processor main/aux layout fixture audio | 2.98023224e-8 |
| Independent interrupted monitor-weight/audio fixture | 5.55111512e-17 |
| Raw-key monitor mappings/latency/precedence | 9.21518231e-9 |
| Complete processor interrupted monitor/source/mode/connection schedule | 1.20245814e-8 |

The complete processor monitor oracle corrects the generated fixture only for
the existing host parameter quantization of output/wet gain to 0.1 dB. Its route
weights use independently transcribed closed-form event segments. The pure
monitor test reproduces the generated exact-gain fixture directly. An initial
switch-test mismatch was corrected by distinguishing completed-hop control
events from first-input-sample connection events; DSP and tolerances stayed.

Clang builds with project `-Wall -Wextra -Wconversion -Wshadow` produced no source
warnings. Spectral AU, VST3 and standalone products built. The pre-existing
no-install cache redirected bundles exclusively to `build/fixxy-brand-staging`.
Fresh staged VST3 pluginval strictness 5 finished `SUCCESS`, exit 0, including
non-main bus enable/disable and restoration tests. Log:
`/tmp/sidechain-pluginval.log`. Fresh AU `auval` was not run against this staged
bundle; no installed Audio Unit was replaced or claimed as fresh validation.

## Explicit audio-thread audit

- **Allocation/deletion:** key storage, delay rings and scratch allocate only in
  prepare. No vector growth, resize, new/delete or owning-buffer destruction in
  process. Prepared node and processor callback instrumentation observed zero
  new/new[]/delete/delete[] calls across fixtures, oversized chunks, source/bus
  changes and monitor transitions. This instruments C++ heap operations; the
  existing vDSP implementation's prepare-only setup is additionally source-audited.
- **Locks:** none added; node is atomic-free. Processor meters use relaxed scalar
  atomics and existing bounded publishers. Bus views are non-owning small views.
- **I/O/logging:** none in callback. Existing `mach_absolute_time` diagnostic
  counter reads and relaxed numeric publication remain; logs are test-side only.
- **Bounded work:** at most two physical key FFTs per completed hop, only when
  external detection is enabled. History ingestion stays bounded at two channels,
  1024 samples and 513 bins. Key capture/delay/meter and four monitor candidates
  are fixed work per sample. Oversized host blocks split into prepared chunks;
  callback storage never grows. Existing envelope/sort and coefficient work
  remain fixed-bin bounded. No CPU deadline guarantee is inferred from this audit.

## Native signal evidence and remaining limitation

`SpectralProcessorTest --snapshots proto/out/sidechain-port-ui` finished exit 0.
The native editor received actual stereo main and separate physical key tones;
the physical meter exceeded -20 dB. Inspected screenshots:

- `proto/out/sidechain-port-ui/spectral-sidechain-minimum.png` (892×520).
- `proto/out/sidechain-port-ui/spectral-sidechain-collapsed.png`.
- `proto/out/sidechain-port-ui/spectral-sidechain-detection.png`.

SC/listen toggle state, live key spectrum, reduction, independent detection and
collapsed layout are visible without overlap. A small presentation issue was
reported to the lead: `juce::String(value, 0)` rendered excess meter decimals and
shrunk the 52-pixel label. The lead changed it to one decimal after these captures;
the next combined cap/product verification owns the rebuilt visual check. This
report records the inspected pre-formatting-fix state. No commits performed.
