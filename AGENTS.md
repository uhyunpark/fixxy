# AGENTS.md

Act as a DSP practitioner and real-time systems engineer.

## The Project

Build a **meta-plugin**: a DAW plugin that constructs and reconfigures its own DSP graph
from natural-language requests. Getting there requires building the parts first — each
classic effect (EQ, compressor, saturator, delay, spectral processor) is both a learning
exercise and a node in the eventual graph library. Project overview and build/test
commands: `README.md`. Current stage and full status: `docs/soothe3-parity-progress.md`.

## Who I Am

- **Experienced** in software architecture, systems design, and C++/build tooling.
- **Beginner** in DSP and audio programming.

So: explain the *why* behind DSP concepts. Skip explanations of CMake, C++ idioms,
git, or general software patterns — I already know them.

## How to Work With Me

- **Claude orchestrates; I instruct and review.** All implementation work goes through
  subagents — always. Claude's job is decomposition, precise agent instructions,
  verification gates, and synthesis. Do not implement in the main context except
  trivial single-line conversation-level fixes.
- **Model selection is explicit, per task**: opus for DSP math derivations and
  audio-thread-critical code, sonnet for mechanical implementation against a fixed
  spec, haiku for chores. Never let an agent silently inherit an expensive default.
- **Prototype in Python first.** New DSP goes research -> numpy proto with
  assert-based verification against an independent reference (scipy) -> then C++.
  A derivation without a numeric cross-check does not enter C++ (a research-stage
  formula error has already been caught this way — see docs/architecture.md 함정).
- **Audit the audio thread.** Any code touching processBlock() gets an explicit
  allocation/locks/IO/logging/bounded-work audit in the agent's report *before* merge.
- **Gates before commit**: clean build with our warning flags, console tests pass,
  pluginval strictness 5 SUCCESS; UI-facing changes additionally get a visual check
  with real signal in the standalone app or a DAW. Commit only when I say so.
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

JUCE + CMake + Ninja + Clang, Python proto — see README.md and
docs/architecture.md 결정 사항.

## See Also

- `README.md` — project overview, local build and test commands.
- `docs/architecture.md` — architecture, decisions, 함정 (traps), historical roadmap.
- `docs/soothe3-parity-progress.md` — current status and verification checkpoints.
- Folder `AGENTS.md`s (`src/dsp/`, `src/plugins/`, `proto/`, `test/`) — conventions,
  loaded on demand by whoever is working in that directory.
- `docs/` — domain reference guides (EQ, compressor).
- `.claude/skills/dsp-pipeline/SKILL.md` — the research -> proto -> C++ port process.
