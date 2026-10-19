# LpngSpectral workflow parity plan

This plan follows the working sequence described in the official soothe3 manual,
without copying soothe3's appearance. LpngSpectral keeps its existing cream,
ink, azure, and ember language: the spectrum remains the visual centre, ember
means processing action, azure means signal/control data, and toolbar chrome is
neutral.

## Supported workflow

- The header provides a named preset menu, undo/redo, A/B selection, copy from
  the active slot to the other slot, sidebar collapse, and context help.
- A and B are real processor-owned parameter snapshots. Switching first captures
  the active slot, then restores the destination through host-notifying parameter
  gestures. Copy replaces the inactive slot without changing the sound.
- Preset files contain a parameter-only `PARAMETERS` tree. Factory baseline is a
  processor-construction snapshot of parameter defaults. Loading a preset or the
  baseline changes the active A/B slot as one named undo transaction.
- The side panel can collapse to a narrow strip. Its three indicators show when
  stereo, tilt, or scale parameters differ from their defaults.
- Help is an in-editor quick-start panel, based on the manual's sequence: set
  depth, refine detail, audition delta, shape the depth curve, then compare with
  bypass. Existing tooltips remain the control-level help.
- Selected-band controls expose only declared parameters: enabled/bypassed,
  shape, frequency, depth weight, and Q. No listen, channel assignment, slope,
  mode, quality, or sidechain controls appear until those services exist.

## Processor service contract

`juce::UndoManager undoManager` must be declared before `apvts`, and APVTS must
be constructed with `&undoManager`. A processor-owned `SpectralWorkflow workflow`
must be declared after `apvts` and constructed from `(apvts, undoManager)`. This
ordering lets attachments and bulk workflow operations share the same history
while A/B state survives editor recreation.

`SpectralWorkflow` owns:

- the factory-baseline parameter snapshot;
- parameter-only A and B snapshots and the active-slot index;
- the current preset name and its clean snapshot;
- the expanded/collapsed side-panel preference.

Snapshot restore never calls `AudioProcessorValueTreeState::replaceState()` for
an interactive action because JUCE clears the undo history there. It flushes
pending values with `copyState()`, begins one named transaction, and updates each
changed `RangedAudioParameter` using `beginChangeGesture()`,
`setValueNotifyingHost()`, and `endChangeGesture()`. A final `copyState()` records
the APVTS ValueTree changes in the same transaction.

Processor host-state serialization uses a `LPNGSPECTRAL_STATE` wrapper with
sibling `PARAMETERS` and `WORKFLOW` children. `WORKFLOW` contains the two flat
parameter snapshots, active slot, preset identity, and side-panel state. The
processor must still accept the legacy bare `PARAMETERS` root. Workflow metadata
is removed before APVTS replacement so snapshots cannot recursively contain
session state.

The first version uses JUCE's one APVTS `UndoManager`. A slot switch clears that
history after restoring the destination so undo can never apply an edit from the
wrong slot. JUCE does not support swapping or cloning the manager attached to
APVTS; fully independent retained undo stacks for A and B require a later history
layer. This is a known parity gap from soothe3's independent per-slot histories.

## Band lifecycle boundary

`wbandNOn` does not represent both retained bypass and deletion. A separate
presence bit per band lives in workflow metadata and in the parameter preset's
root metadata. Legacy loading infers presence from `On` or any non-default band
parameter. A positive `On` edge from host automation is recorded in a lock-free
atomic mask, then merged into retained presence on the next non-audio workflow/UI
snapshot; an automated true-to-false pulse therefore cannot disappear between
queries. Add allocates the first absent band, bypass preserves all parameters and
draws a hollow selectable node, and delete clears presence then resets every band
parameter with DSP `On` left false. Add and delete each form one undo transaction
spanning metadata and APVTS changes. Preset validation also requires canonical
integer values for boolean and choice parameters before making any mutation.

## Deferred DSP-backed parity

The reduction display should ultimately show the audible wet contribution after
Mix, and stereo-unlinked reduction should expose both channel traces. Processing
mode, band listen, sidechain, quality, and channel assignment also require real
DSP/schema services. This UI phase does not simulate any of them.

Reference: [official soothe3 manual](https://oeksound.com/manuals/soothe3/).
