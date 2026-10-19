# fixxy Spectral: functional audit and research handoff

2026-10-17. Stage 1 research; source inspection only, no new product validation.
Snapshot: current dirty worktree, including the ongoing fixxy branding changes.
This audit supersedes the earlier **scope exclusion** of surround; it does not
supersede verified numerical fixtures. The requested all-feature target includes
multichannel. No proprietary algorithm or sonic-equivalence claim is supported.

Primary reference: [official manual](https://oeksound.com/manuals/soothe3/),
last modified August 4, 2026, inspected October 17. Section numbers below are
traceability pointers, not an assertion that our candidate formulas are official.
The [product specifications](https://oeksound.com/plugins/soothe3/) additionally
identify 9.1.6 support, macOS AU/VST3/AAX and Windows VST3/AAX. These references
describe observable operation; detector mathematics, exact response curves and
transition scheduling remain undisclosed.

## Code map and status vocabulary

All locations are relative to the repository root, at the inspection snapshot.
`N` = `src/dsp/SpectralNode.h`; `C` = `src/dsp/SpectralControl.h`;
`W` = `src/dsp/SpectralWeight.h`; `S` = `src/dsp/SpectralSchema.h`;
`P` = `src/plugins/spectral/PluginProcessor.cpp`;
`F` = `src/plugins/spectral/SpectralWorkflow.h`;
`E` = `src/plugins/spectral/ui/SpectralEditor.h`;
`B` = `src/plugins/spectral/ui/WeightBandLayer.h`;
`V` = `src/plugins/spectral/ui/SpectralAnalyzerView.h`.

**Present** means wired, not newly tested. **Partial** means important behavior
differs. **Primitive** means independently checked math exists but is not a
product feature. **Absent** means no product implementation found. Each gate is
our proposed independent acceptance experiment, not a manufacturer test.

## Traceability matrix

| Domain / reference | Current source evidence | Required verification / remaining implementation | Original-only unknown |
| --- | --- | --- | --- |
| Soft / Hard / Depth (§3) | **Partial**: N:578 uses `hardReduction`; N:594 uses relative median/tanh; S:49 and `detectionMode` expose them. | Render identical signals at several global scales; Soft's steady reduction should remain stable above the numerical floor, Hard should vary with absolute level; sweep Depth monotonically. Preserve legacy Soft fixtures. | Threshold, knee, adaptive detector, maximum-detail distortion and actual musical transfer curves. |
| Detail (§3) | **Partial**: N:83–91 width anchors increase from Detail 0 to 5; strength decreases from 5 to 10. Hard uses width but lacks separate detail detector behavior. C has candidate monotonic maps. | Sweep one held tone on a broadband bed and measure notch width/depth; separately measure detector settling. Do not call the current nonmonotonic anchors full compatibility. | Precise width/strength map and Soft detector delay. |
| Attack/release (§3) | **Partial**: S:72/83 expose milliseconds; N:286 uses frequency-flat default poles. | Frequency-dependent normalized-control successor needs a versioned migration. Measure step/burst response at 100 Hz, 1 kHz, 8 kHz, several rates and hop sizes against closed-form pole references. | Actual per-frequency timing and knob calibration. |
| Six independent tilts (§4) | **Primitive**: C:19–61 has low/high shelf profiles; product N:270/290/298 and S:180–203 have only three bipolar tilts. Detail tilt is Soft-only. | Six independent controls must each move the intended local width/strength/detector/timing field; test low-only/high-only isolation and hover-region display. | Shelf slopes, extent and exact scaling. |
| Stereo mode, link, focus (§4) | **Partial**: N:239–264, 407–473 implement M/S, focus and **MAX** linking. C:111–125 contains power-domain candidate linking. | Use identical, opposing-polarity, one-sided and independent stereo probes. Pin dual/shared endpoints, M/S identity, focus direction and automation continuity. A MAX statistic cannot be described as summed analysis. | Summation domain/normalization and intermediate linking law. |
| Max cut (§4) | **Partial / defect**: N:588/600 caps demand **before** retained envelope states; N:633 emits unrestricted `y`. `>=24` is Off. | Lower cap during a long-release deep notch and inspect every final gain bin. Add the post-ballistics cap handoff below; distinguish a frame-gain bound from arbitrary audio attenuation. Draw requested/effective limit truthfully. | Manufacturer automation ramp and final-audio cap definition. |
| Wet trim / Mix / Output (§4,6) | **Present algebra**: `SpectralMix.h:51`; P:440–448 uses smoothed gains and aligned dry. | Independent scalar oracle at Mix 0/25/50/100, positive/negative trim and output; normal, delta and bypass paths; host-state restore. | Delta trim inclusion/polarity requires original capture. |
| Bypass / Delta (§6) | **Partial**: P:325–351 and `SpectralMix.h` keep processing under bypass; selection is discrete. Delta is wet-minus-dry. | Null delayed dry under bypass; null Delta at Depth zero/trim zero; transition-click test with a sustained tone. Add monitor fades with a declared independent policy. | Exact switching fades and Delta conventions. |
| Eight bands and lifecycle (§5) | **Present core**: W:36 has eight; F:242/255 separates presence and enable; B:259 double-click toggles. | Eight-create/ninth-reject, bypass-preserves-settings, delete-frees-slot, undo, host automation and recall. | Exact defaults by insertion position. |
| Band shapes, Q/slope and text (§5) | **Partial**: W:94 fixes low/high cuts at 12 dB/oct; S:30 offers eight corresponding shapes; P:238 and E have no special frequency parser. | Implement shape-dependent slope control; test finite responses and supported slope values against shared response evaluator. Parse `6300`, `6k3`, `6.3k`, `6,3k` equivalently; reject malformed input atomically. | Proprietary slope range and band response family. |
| Depth-curve routing / bandpasses (§5) | **Partial**: N:648–672 sums every band's dB, including bandpasses, so disjoint bandpasses behave like serial restrictions. C:128 offers a verified candidate union. | Two disjoint selected regions must both retain suppression; test overlap, bypass, masks after smoothing, cut/range skirts, and exact UI/audio response agreement. | Parallel-combination law and exact skirts. |
| Per-band focus / assignment (§5,8) | **Absent**: W `WeightBand` has no focus or channel-set fields. | Add versioned per-band targets and verify that a positive/negative depth curve affects only assigned lanes; switching L/R↔M/S must update labels and behavior together. | Exact focus interpolation and routing interaction with global focus. |
| Band audition (§5,7) | **Absent**: no band-listen signal path in P or N. | Audition selected-region Delta and solo through a separately verified monitor filter; enabling listen must not change reduction computation. Hold/release Alt and optional drag-listen restore previous monitor state. | Filter bandwidth/shape and multi-selected audition policy. |
| Node creation/context (§5) | **Partial**: B:179 popup acts only on an existing node; B:259 creates using the free slot's existing shape. | Empty-graph type menu, edge-sensitive low/high cut insertion, Cmd/Ctrl bandpass insertion; menu and band-strip edits must produce the same state. | Edge threshold and drag feel. |
| Selection and modifiers (§5) | **Absent/partial**: B:672 stores one index; B:211 absolute drag, B:285 unmodified Q wheel. | Box selection, Shift toggles, group editing/bypass/delete with one undo transaction; fine drag/wheel, Q drag, focus wheel, audition modifier, text reset; simulate gestures and verify host begin/end pairs. | Exact multi-edit clamping behavior and sensitivity. |
| Reduction/depth display (§5,6) | **Partial**: N:464 publishes only the deeper channel; V:302 draws raw reduction, receives neither Mix nor wet trim. | Display separate active lanes and actual mixed response, verified against steady measured audio; cap shading and selected-surround-channel display; Mix=0 must remove normal-mode reduction. | Visual interpolation/smoothing and transient display delay. |
| External sidechain / meter / listen (§7) | **Absent**: P constructor has main input/output only; N detection is main spectra. | Follow separate-source handoff below; distinct meter, enable and listen; test connected/silent/absent keys, both modes, all channel layouts and bypass precedence. | Mono/stereo normalization and missing-source semantics. |
| Normal/High/Ultra quality (§6) | **Absent**: N:75–76 fixes N1024/H256; E:43 explicitly omits quality. | Quality must produce a measured cadence/accuracy difference; include full automation CPU tests, not a relabeled switch. Preserve independent quality preset lock. | Proprietary FFTs, cadence and effective resolution. |
| Minimum/linear phase (§3,6) | **Primitive**: `SpectralFir.h` minimum-phase candidate and `SpectralTransition.h` are disconnected; linear-phase recipe only in `proto/spectral_parity.py`. | Static measured magnitude/phase, causal/minimum-phase evidence and symmetric linear-phase impulse; dynamic cap and history checks. WOLA gain masks do not establish minimum phase. | Original finite-filter design and phase transitions. |
| Low latency / host latency (§6) | **Absent**: N:342/P:275 report fixed 1024. | Causal unity impulse and host reported latency at each mode/rate, including transitions; low latency forces its actual supported quality/phase. Add independent low-latency preset lock. See published latency reference below. | High-rate resampling/filtering details. |
| A/B and copy (§7) | **Present**: F:94/109 owns state; processor persistence includes both slots. | Separate state, identity/presence/clean baseline, copy direction, editor close/reopen and host serialization. | Copy's treatment of monitor/global settings. |
| Undo/redo (§7) | **Partial**: F:101 and 588/625 clear the sole UndoManager on A/B switch. | Keep a bounded independent edit history per slot; edit A, edit B, return A and undo A without mutating B. Include multi-node edits, preset metadata and dirty baseline. | History capacity and copy transaction policy. |
| Preset files and dirty state (§7) | **Present core**: F:149/170 validates XML; E:503 only baseline/open/save-as; E:587 dirty asterisk. | Preserve atomic malformed-file rejection and legacy migration; add Save distinct from Save As, user metadata/category editing, protected factory content and delete confirmation/recovery behavior. | No proprietary preset-format compatibility target. |
| Preset browser (§7) | **Absent** beyond file chooser. | Search name/category, category and factory/user/favorite filters, selected preview/description, immediate load, previous/next, arrow keys, double-click/Enter/Escape close. Test with our own fixture preset library. | Manufacturer factory preset contents are excluded. |
| Favorites / startup default (§7) | **Absent**: no persisted preset catalog/preferences. | Favorite persistence and filtering; select/reset user default and verify new instances use it; surviving files after relocation/deletion must not silently corrupt identity. | Factory names/descriptions are excluded. |
| Global settings / offline (§7) | **Absent**: no global preference service or `isNonRealtime` quality selection. | Shared-instance preferences for tooltips, drag audition, Delta audition, cursor frequency; offline Ultra override except low-latency instances; global low-latency override must survive preset loads. Test two instances and realtime/offline transitions. | Host-specific offline notification behavior needs host testing. |
| Sidebar / resizing / accessibility (§4,7) | **Present/partial**: E:465 collapse, E:630 changed indicators, E:245 drag resizing. No size presets/global tooltip preference/cursor frequency preference. | Narrow/wide screens, preset sizes rejected when unavailable, keyboard and readable focus states; visual signal check at every supported size. | Exact layout and proprietary artwork are excluded. |
| Surround layouts and band targets (§8) | **Absent**: P:318 rejects anything beyond mono/stereo; N:77 fixed two channels. | Support channel-identity-based layouts through 9.1.6; sets LR, C, LFE, surround, ceiling; per-band set/pair assignment; silent-lane impulse mapping, no channel reorder/leakage. | Format-specific supported layout variants need host/reference enumeration. |
| Surround linking/depth (§8) | **Absent**. | Independently model pair link, group link and link-all selection; group/link-all are exclusive. LR+C grouped together. Set depth changes both suppression and linked-analysis weighting. With pair link below full, left/right differences must survive full cross-set linking. Test a literal small-graph reference before C++. | Summation/normalization and exact intermediate equations; do not invent them as measured facts. |
| Surround monitoring/bypass/meters (§8) | **Absent**. | Set bypass/solo/Delta, pair/individual bypass via modifier, per-channel maximum-reduction meters, pair hover and individual modifier-hover graph with labels. Probe unique tones in all 16 channels and verify every selection. | Monitor gain/normalization and simultaneous listen precedence. |
| Help / legal / support / updates (§7,9,10) | **Partial**: E:688 context help only. | Supply our own manual, legal notices, system-info copy and useful support route. Independently version our preset storage/docs. An update indicator needs a real fixxy release source; omit until one exists. | Manufacturer support, update service, licensing and preset assets are excluded. |
| Packaging / platform breadth | **Partial**: CMake AU/VST3/Standalone on current Mac; no AAX target. | Independently branded fixxy binaries with stable legacy plugin IDs; validate chosen platform/format matrix explicitly. AAX distribution and Windows validation need their own build/host resources. | Commercial licensing/install infrastructure is not a DSP clone requirement. |
| Host automation/state/RT cross-cutting | **Partial**: P:375 explicitly bypasses smoothing for link/focus/tilts/weights; F retains versions 1/2. | Smooth continuous transitions, migrate new IDs without changing old renders, test active transport restore, all layouts/rates/blocks and offline. Audit no allocation/deletion/locks/I/O/logging and bounded callback work. | Original automation scheduling cannot be inferred from static documentation. |

The detailed acceptance experiments above are our engineering specification.
They must not be mistaken for recovered manufacturer algorithms. For concise
observable definitions and interaction details use the cited manual directly.

## Latency and evidence boundaries

Published sample counts (manual §6): regular / low-latency / linear phase are
2304 / 0 / 3456 at 44.1 and 48 kHz, 4704 / 96 / 7008 at 88.2 and 96 kHz,
and 9408 / 192 / 14016 at its highest-rate pair. The manual prints **174.4**
kHz beside 192; do not silently turn that apparent typo into a verified 176.4
kHz original measurement. Verify 176.4 independently when available. Quality
selection itself does not authorize inventing latency; report our measured
algorithmic latency. A target's latency can be matched by a justified delay
only after its actual processing fits within that budget.

There are 19 old WAVs under `proto/out/soothe3_kit/out/` and a 44.1 kHz input
manifest. They are useful existing evidence, not unavailable data. Directly
measurable now: file identity/rate/channel/length/finite samples, residual
alignment versus the no-plugin/bypass bounces, steady tone attenuation, noise
spectral ratio, burst envelope, notch-probe widths and Delta algebra for the
matching probe. Analyze both channels; the old analyzer's `read_mono()` only
examines channel zero. Keep simulated `selftest/out/` completely separate.

Their limits are concrete: `docs/soothe3-measurement-procedure.md:25` leaves
actual Depth blank, uses percentage Detail labels, and leaves timing/host
latency values blank. There is no per-bounce parameter readback, default-band
state or offline-quality/global-preference snapshot. The manifest describes
inputs, not plugin state. A PDC-compensated zero-lag bounce cannot establish
plugin-reported latency. Noise Welch ratios do not identify a unique static
transfer function for a time-varying suppressor, and a fitted envelope constant
includes detector/window/lookahead effects. These bounces can support an
archived-case comparison; they cannot certify control calibration, new features,
all-rate phase behavior or perceptual equality on music. Do not discard them,
but do not tune a contradictory Detail law to ambiguous filenames.

New original comparisons require a legitimately working reference with saved
state/readback, plugin version, all global/offline settings, sample rate, layout,
PDC mode and source/output hashes. Independent implementation validation can
continue before those captures exist. Identical algorithms remain unprovable
from finite black-box observations even with excellent recordings.

## Immediate Max cut handoff for the existing WOLA engine

Reuse the already verified `proto/spectral_parity.py:124` and
`SpectralControl.h:94–109`. There is a `smoothCap` function, not a
`CapSmoother` class. At each **actual internal hop**, once shared by all lanes:

```
a = exp(-H / (fs * 0.020))
effectiveCap = a * effectiveCap + (1-a) * requestedCap
releaseState = max(target, releasePole * (releaseState-target) + target)
y = attackPole * (previousOutput-releaseState) + releaseState
previousOutput = min(y, effectiveCap)
publishedReduction = previousOutput
gain = pow(10, -previousOutput/20)
```

Clamp after frequency smoothing and ballistics, and store the clamped output.
Keep release memory unchanged: the existing verified primitive does so; also
clamping it changes recovery behavior. The old pre-knee/target cap may remain
for compatibility, but is insufficient by itself. The smoother is a **fixxy
policy**, not an identified original ramp.

Preserve stable-Off arithmetic exactly. During enable/disable transitions map
the legacy `maxCut>=24` Off sentinel to a proven internal demand ceiling:
`max(40, kMaxDb * (10/5) * 4)`, approximately **129.341083 dB**. The factor 4
is the maximum legacy Detail-tilt multiplier at N:273; a 40 dB surrogate would
incorrectly cap valid old Soft settings. Start enabled fresh instances at the
requested cap, Off fresh instances at that ceiling. Retained release history
also stays within this valid-parameter bound. A finite transition-completion rule
must be tested: proposed residual <=1e-6 dB followed by an exact snap to target;
stable Off then bypasses the clamp. This tolerance is newly proposed, not
already covered by the primitive fixture. Do not change the public sentinel
in this bounded bug fix; an explicit enabled flag belongs to a versioned
successor interface. A later enable must ramp from the current effective state.

The proof obligation is `0 <= reduction[frame,bin] <= effectiveCap[frame]`,
therefore every applied spectral gain has a lower bound. Old WOLA frames are
already in overlap-add output; per-frame gains do **not** prove an instantaneous
sample-domain minimum gain, or a universal final-audio attenuation limit for
time-varying audio. Publish or document this limit honestly. Align any effective
cap display to the reduction frame it describes.

Stage 2 gates: deep sustained notch followed by falling/rising cap with long
release, including extreme legacy Detail tilt above 40 dB demand; both detectors,
both lanes and M/S; cap changes at non-hop boundaries;
Off bit identity; startup/enable/disable behavior; independent scalar recurrence
and offline STFT reconstruction; discontinuity/sideband measurement. A passing
per-bin assertion alone is not a listening verdict.

## External sidechain handoff for the existing WOLA engine

This is our explicit routing choice where the manual leaves details unspecified.
Add an optional disabled/mono/stereo input bus and separate prepared FIFO,
analysis rings and spectra on the same N1024/H256 clock. Main spectra always
carry the processed audio. `getBusBuffer` must separate key inputs from main
outputs before any in-place write. No audio-thread resizing.

* SC disabled: preserve the existing main-detection path bit-for-bit.
* SC enabled: use the selected key for **both** Soft and Hard. Keep warm key
  history while a source is connected, even when SC detection is disabled.
* Mono key: analyze once, broadcast calibrated magnitude to both detector
  lanes, including M/S. Duplicating the waveform then M/S-encoding would erase
  its Side detector; that is not the intended broadcast.
* Stereo key/stereo main: transform actual key waveform into M/S when selected,
  otherwise L/R. Retain current linking in this bounded increment; mark MAX
  linking partial until the independently verified summed-link successor lands.
* Stereo key/mono main: use verified RMS magnitude sum
  `sqrt((aL*aL+aR*aR)/2)` for analysis; phase cancellation is avoided. This is
  our normalization choice, not original evidence.
* Missing or silent key: zero new suppression demand through existing release;
  never fall back to main. In Soft, zero exceedance is insufficient because
  its knee has a nonzero baseline: skip the knee demand explicitly. A newly
  disconnected source must have a declared stale-ring policy; proposed policy
  zeros its detector demand immediately while the separate release state decays.
* Listen is independent of SC enable. Audition latency-aligned physical key
  audio: mono duplicated to stereo, stereo to mono as `(L+R)/2`. The latter
  is an audio downmix, deliberately different from RMS detector aggregation.
  Apply Output only, ignore Mix/wet trim; missing key yields silence.
* Priority: bypass > SC listen > selected-band listen > Delta > normal.
  Proposed monitor crossfade is 5 ms; verify before porting. This is a designed
  policy, not an undisclosed property of the original.

Gate with separately generated main/key frequencies, fixed key while main level
changes, fixed main while key changes, silence/disconnection, mono key in M/S,
anti-phase/M-only/S-only stereo keys, both detection modes, each routing layout,
host oversized/odd chunks, raw-listen delay/null and monitor transitions. Use a
SciPy STFT/ISTFT oracle driven by independently supplied key spectra and assert
that key samples never enter normal audio output. SC Off must retain existing
Soft fixtures. Extend host/schema/state migration and real-signal visual checks.

## CPU decision: bounded probe before renderer integration

The September 22 transition report's rejection stands. Complete scoped stereo
vDSP automation took **2230 us mean at H64/48 kHz**, versus a **1333 us** hop
deadline; all tested 96/192 kHz cadences also failed mean throughput. Static
primitive success and zero allocations do not solve this. A worker moves work
but does not create throughput; asynchronous target age must be bounded.

Do one targeted performance experiment, using the existing benchmark rather
than rewriting the renderer:

1. Scale H with the existing sample-rate capacity multiplier: base H=256/128/64
   at 44.1/48 kHz, doubled at 88.2/96 and quadrupled at 176.4/192. Preserve
   update duration rather than demanding four times more updates at 192 kHz.
   This corrects an unnecessarily expensive candidate schedule, not original
   parity. Log analysis, per-bin coefficient setup, FIR synthesis, partition
   refresh/history initialization and rendering independently.
2. Compare unchanged controls and all-control automation. Candidate optimization:
   cache frequency-static factors and update expensive control coefficient
   tables at a bounded 100 Hz clock, interpolating between prepared values;
   detector and filter-update quality clocks remain separate. Verify against
   a direct per-hop oracle with a declared audio/control error budget before
   adopting interpolation. Do not silently drop or backlog automation events.
3. Measure actual callback bursts (32/64/128/256/1024 and odd sizes), 44.1–192
   kHz, scalar and vDSP, sidechain, all bands and **16 unlinked channels**.
   Reuse linked kernels/analysis only when routing/depth/focus are identical.
   Record p99/max and total target age; an average or hop deadline alone misses
   small-callback bursts. Require explicit multi-instance CPU headroom, not
   merely a sub-100% single-instance figure.

If those bounded changes cannot pass, stop integrating this FIR design. The
next candidate is a fixed-capacity dynamic minimum-phase IIR notch/shelf bank,
with bounded peak selection/fitting at a separate control cadence. This could
reduce synthesis cost but sacrifices arbitrary dense target accuracy; the
number of sections, fitting-error suite, stability and cap-safe transitions
must be researched/prototyped independently. It is not an already approved
shortcut. Keep the proven WOLA engine explicitly identified until a replacement
passes. Neither a WOLA mask nor removing a host latency report creates true
minimum-phase/zero-delay processing. A multirate audio path also needs measured
resampler response/delay; it cannot be waved in as zero latency.

## Implementation order and root decisions

1. Fix the present cap defect and add independently verified sidechain/listen
   to the existing engine; keep legacy Off/SC-Off renders exact. These are
   useful deliverable increments independent of the FIR decision.
2. In parallel, repair A/B histories and implement a real independent preset
   catalog/browser/defaults/favorites; complete node multi-selection/modifiers
   that have implemented parameter destinations. No decorative DSP controls.
3. Version the successor sound model: six tilts, monotonic Detail, normalized
   timing, summed link, per-band focus, masks/slope, band audition and truthful
   separate/mixed curves. Root must choose migration behavior: old sessions
   retain legacy engine semantics; new behavior should not silently retune them.
4. Run the bounded renderer probe and decide CPU/target-age budgets before
   minimum/linear-phase, quality or low-latency UI. Locks/offline preferences
   ship when their modes have actual implementations.
5. Generalize channel identities and routing through 9.1.6 with a separate
   Python link-network oracle, then surround controls/meters. This is in scope;
   it cannot be marked complete by stereo validation. Root must choose explicit
   format/platform support and acknowledge unavailable AAX/Windows host gates.
6. Warning-clean build, all relevant console suites, pluginval strictness 5,
   AU validation, heap/lock/I/O audit, real-signal native visual checks and
   representative listening. Reference comparison remains a separate evidence
   gate; record unavailable captures without calling full sonic parity complete.

No commits, source edits, original-plugin activation attempt or redundant full
test run were performed for this audit.
