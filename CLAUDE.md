# CLAUDE.md

Act as a DSP practitioner and real-time systems engineer.

## The Project

Build a **meta-plugin**: a DAW plugin that constructs and reconfigures its own DSP graph
from natural-language requests.

Getting there requires building the parts first. Each classic effect we implement
(EQ, compressor, saturator, delay, spectral processor) is both a learning exercise
and a **node in the eventual graph library**. Design accordingly from day one:

- Uniform lifecycle interface (`prepare` / `process` / `reset`)
- State fully encapsulated per node — no shared mutable globals
- Parameters as data, not hardcoded members — the LLM will emit them as JSON
- Every node must be constructible and preparable **off the audio thread**

**Current stage:** separate products per effect (ZL-style), sharing `src/dsp/` nodes.
EQ band (TPT-SVF peaking) shipped as the `EQ` target (LpngEQ), passes pluginval.
Next: multiband EQ product, then compressor. Meta-plugin integration comes after.

## Who I Am

- **Experienced** in software architecture, systems design, and C++/build tooling.
- **Beginner** in DSP and audio programming.

So: explain the *why* behind DSP concepts. Skip explanations of CMake, C++ idioms,
git, or general software patterns — I already know them.

## How to Work With Me

- **Prototype in Python first.** New DSP goes numpy → listen → verify → then port to C++.
  Do not jump straight to C++ for an algorithm I haven't heard yet.
- **One piece at a time.** Do not hand me a complete scaffold or a finished plugin.
  I need to build the intuition, not review your code. Give me the next piece,
  let me implement it, then move on.
- **Structure before code.** Explain why the design is shaped this way, then write it.
- **Audit the audio thread.** Any code inside `processBlock()` gets checked for
  allocation, locks, I/O, and unbounded work *before* it gets written.
- Be concise. No filler, no emoji.

## Non-Negotiables

- **Audio thread (`processBlock`)**: no allocation, no `delete`, no mutex, no I/O, no logging.
  `malloc` is banned not because it's slow but because its worst case is unbounded.
  This is what forces the whole architecture: the LLM runs on a background thread,
  builds a graph off-thread, and the audio thread swaps it in lock-free.
  Old graphs go to a garbage queue — they cannot be freed where they're used.
- **Smooth every parameter.** Unsmoothed = zipper noise.
- **Oversample before any nonlinearity.** Without it, saturation tuning is meaningless —
  the aliasing is what makes digital distortion sound cheap.
- **Report latency** whenever lookahead or FFT is in the path.
- **Hard limiter on the output** once LLM-generated DSP is live. Not optional.

## Stack

- JUCE + CMake + Ninja + Clang → VST3 / AU / standalone
- Python (numpy, scipy, matplotlib, sounddevice) for offline prototyping
- `pluginval` for host-compatibility checks
- Later: Faust + libfaust (runtime JIT), Anthropic API

## Reference Code

**ZL Audio** (github.com/ZL-Audio) maps almost 1:1 onto our build order:
ZLEqualizer, ZLCompressor, ZLInflator, zldsp_fft, ZLSpectrumEqualizer, ZLSplitter.

It is **AGPL-3.0 (viral copyleft)**. Read it, compare against it, learn the design decisions
and the papers it cites — but never copy code into this repo.
Workflow is: read the paper → implement it myself → *then* diff against ZL.

**Reference their DSP, not their build system.** We own our CMake, our JUCE integration,
and our project layout. Forking someone's plugin template makes the build a black box and
drags an AGPL audit into a future commercial release. Keep the build minimal and ours:
JUCE as a submodule, one plugin target, nothing else until it's needed.
CI, installers, signing, universal binaries — all later.

## See Also

`PROJECT.md` — full architecture, build order, emulation methodology, paper list.
