# soothe3 reference host report

Date: 2026-09-09

## Result

`ReferencePluginHost` is a bounded JUCE console host for public VST3/AU interfaces. It
can enumerate parameter IDs/names/default display mappings, report latency after
prepare, apply normalized parameters by ID or normalized name, and render 32-bit
float WAV files offline. Transport is deliberately absent (`AudioPlayHead == nullptr`).

The installed soothe3 VST3 was discovered as `soothe3`, version 1.0.5 according to
its public bundle metadata, but its module did not finish loading. The host's serviced
JUCE/CoreFoundation event loop blocked in the bundle initializer, inside the PACE
wrapper waiting in `recvfrom`. The 30-second watchdog exited with status 124. JUCE
therefore never returned an `AudioPluginInstance`.

Consequences:

- soothe3 Depth, Detail, Attack, Release, quality/mode, low-latency parameter IDs,
  ranges, defaults, and display mappings remain **unknown**.
- soothe3 latency at 44.1 and 48 kHz remains **unmeasured**.
- The public host interface exposes no licence status. The PACE wait is evidence of a
  protection-service initialization block, but is not proof of a particular licence
  state. No licence UI was opened, accepted, or bypassed.
- Existing soothe3 WAV bounces in `proto/out/soothe3_kit/out` were not treated as a
  clean calibration reference because their full parameter/weight/trim state cannot
  be recovered through the blocked instance.

The Audio Unit registry independently reports `oeksound: soothe3` followed by
`Cannot open component: -1`.

## Reproduction

Build:

```sh
cmake -S . -B build -G Ninja
cmake --build build --target ReferencePluginHost -j 6
```

Inspect:

```sh
build/ReferencePluginHost_artefacts/Release/ReferencePluginHost \
  --inspect '/Library/Audio/Plug-Ins/VST3/soothe3.vst3'
```

Observed stderr:

```text
ReferencePluginHost: scanning /Library/Audio/Plug-Ins/VST3/soothe3.vst3
ReferencePluginHost: found soothe3
ReferencePluginHost: instantiating at 44100 Hz
ReferencePluginHost: timed out after 30 seconds (plug-in scan/load/process may be blocked)
```

Saved diagnostics are under `build/parity/`: `soothe3-inspect.stderr`,
`soothe3-load-stack.sample.txt`, and the empty `soothe3-inspect.json` (no instance was
available from which to produce JSON).

## Control plug-in verification

The same executable loaded the locally built LpngSpectral VST3, processed finite
silence, enumerated 59 public parameters, and reported 1024 samples of latency after
prepare at both 44.1 and 48 kHz. Full output is in
`build/parity/lpngspectral-inspect.json`.

A deterministic offline render applied normalized settings by stable ID/name, disabled
all eight weighting bands, added an explicit 0.1-second zero-input tail, trimmed the
reported latency, and wrote exactly `input + tail` frames. A separate bypass render of
882,000 stereo frames used parameter ID `773352680`; after latency trimming, the output
had the same shape and a maximum absolute sample error of 0 against the source.

Render usage:

```sh
build/ReferencePluginHost_artefacts/Release/ReferencePluginHost \
  --render PLUGIN.vst3 INPUT.wav OUTPUT.wav \
  --set 'Depth=0.65' --set 'Detail=0.35' \
  --tail-seconds 0.1 --trim-latency
```

Output replacement is opt-in with `--overwrite`. Rendering uses a sibling temporary
file and replaces the target only after processing and WAV finalization succeed. The
host rejects a direct or symlink-resolved input/output identity. Checks also confirmed
that an existing output is unchanged when overwrite is omitted, invalid parameter
names and nonnumeric values create no output, and the source WAV hash is unchanged.

Without `--trim-latency`, output retains plug-in delay and contains `input + tail`
frames. With it, the host processes an additional `reported latency` samples, drops
the initial latency, and still writes `input + tail` frames. Tail duration is always
explicit; the host does not guess from `getTailLengthSeconds()`.
