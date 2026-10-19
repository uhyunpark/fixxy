# Spectral UI journal lock fix — 2026-10-19

The per-slot UI journal now records explicit UI transactions outside JUCE
parameter notifications. The final targeted warning-clean build and all **422
SpectralWorkflowTest assertions pass**. No commit or plugin installation was made.

## Defect and correction

The previous journal's parameter gesture/value listeners acquired `metadataLock`
and copied/allocated journal state while JUCE held a parameter's `listenerLock`.
Host restore held `metadataLock`, entered APVTS `valueTreeChanging`, then needed
parameter notification locks. This created a deadlock cycle and could make audio
automation wait behind UI journal work on the same parameter.

`SpectralWorkflow` no longer inherits or registers an
`AudioProcessorParameter::Listener`. UI code explicitly calls `beginUiEdit`,
`markUiParameter(id)` before mutation, and `endUiEdit` after parameter notifications
return. Only marked normalized parameter deltas are replayed; unrelated host
automation, including message-thread automation during a UI group, is preserved.
The existing bounded 64-entry histories, atomic preset/presence operations, v3
state/preset migration, A/B semantics and SC controls remain covered.

Changed source:

- `src/plugins/spectral/SpectralWorkflow.h`: remove journal notification
  listeners; explicitly capture affected IDs; retain the atomic-only band
  presence callback and copied-tree state validation.
- `src/plugins/spectral/ui/SpectralEditor.h`: slider drag/text/reset/wheel hooks
  run outside parameter notifications. Buttons/choice controls use explicit
  gestured setters and silent host-to-UI refresh. Closing ends active parameter
  gestures before committing. Selected-band chip attachments remain bound during
  their drag; pending selection is applied asynchronously after the end callback
  returns. A newer immediate selection clears stale pending selection.
- `src/plugins/spectral/ui/WeightBandLayer.h`: explicitly group node frequency
  and gain, range, wheel, keyboard, shape and enable gestures; close graph/range
  notifications before journal commit.
- `test/spectral_workflow_test.cpp`: deterministic lock and lifecycle regressions
  plus actual UI interaction coverage. Test helpers explicitly model UI origins;
  worker notifications are not treated as UI edits.

## RED → GREEN evidence

Two isolated child-process regressions failed before the fix and pass afterward:

1. Host restore pauses under its first changed parameter notification while an
   actual second-slider gesture enters notification dispatch. Before the fix,
   restore and UI deterministically wait on opposite locks. Afterward both finish.
2. A test ValueTree probe holds the UI journal flush while an audio-like worker
   notifies the same parameter. Before the fix it waits behind journal-held
   notification locking; afterward notification completes while the flush is held.

The parent kills a hung child after five seconds; this is a deadlock watchdog,
not a microsecond performance threshold. A separately strengthened real band-chip
drag/host-removal test reproduced an unmatched old parameter gesture-end before
the deferred rebind guard. It now confirms balanced notifications, no orphan end
on another parameter, hidden removed controls and superseding stale selection.

Final command, completed October 19:

```sh
cmake --build build --target SpectralWorkflowTest -j 2
./build/SpectralWorkflowTest
```

Result: zero emitted source warnings, `passed=422 failed=0`. The real slider
drag/text/reset, footer/mode/SC toggles, band choice/node-pair/wheel/key/range,
editor close, in-flight restore, preset identity/dirty/presence and v3 migration
checks pass. The exact meter expectation preserves the owner's one-decimal
display (`-24.0 dB`); no timer changes or relaxed tolerances were introduced.

## Parameter callback and audio-thread audit

No journal operation executes from a parameter listener. The workflow's remaining
APVTS band-enable listener performs bounded ID checks and one lock-free atomic
bit update: no metadata lock, heap work, state copy, logging, I/O or async posting.
Explicit UI entry points reject worker threads before locking. Journal storage,
snapshots and metadata work remain outside parameter dispatch; undo/restore may
take UI/state locks but parameter callbacks do not acquire those locks back.
No `processBlock`, DSP, schema, parameter IDs or processor source was edited by
this correction. Existing JUCE attachment/message publication behavior is not
claimed to be a new general hard real-time guarantee.

Combined product builds, the full console roster, pluginval/AU validation and
post-fix native real-signal UI inspection remain the release owner's gates.
The preset catalog is paused; its saved new test file is not integrated.
