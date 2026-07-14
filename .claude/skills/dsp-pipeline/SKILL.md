---
name: dsp-pipeline
description: Use when implementing any new DSP feature (filter, compressor, effect, algorithm) — enforces the research -> Python proto -> C++ port pipeline with per-stage subagents, model selection, and verification gates
---

# DSP Feature Pipeline

Every new DSP feature moves through three stages, each a separate subagent. Never skip a stage; never combine stages into one agent.

## Stage 1 — Research (model: opus, background)
- Input: the feature request + relevant papers/docs (check docs/ first; PROJECT.md lists the paper stack).
- The agent reads papers/references (AGPL repos like ZL Audio: READ ONLY, extract design decisions in prose/formulas, never copy code) and returns implementation-ready notes: exact formulas, conventions, magic constants, known traps.
- Output is ground truth for Stage 2 — but it is NOT trusted yet.

## Stage 2 — Python proto (model: sonnet, background)
- Write proto/<feature>.py using .venv/bin/python (numpy/scipy/matplotlib pinned in proto/requirements.txt).
- MANDATORY: assert-based verification against an INDEPENDENT reference (scipy.signal, analog prototype evaluation, closed-form checks). Research formulas have been wrong before (Butterworth cos-vs-sin — caught only by the scipy assert). PASS/FAIL printed with numbers.
- Where audible behavior matters, render WAVs (proto/out/) and plots for the owner to hear/see.
- A failed assert means fix the formula (and report the correction), not loosen the tolerance.

## Stage 3 — C++ port (model: opus, background)
- Port from the VERIFIED proto only — never re-derive.
- Follow the node-library contract (PROJECT.md): prepare(fs, maxBlockSize, numChannels) / reset() / process(); params as data; per-channel state; double precision state/coeffs; JUCE-free under src/dsp/.
- Coefficient recompute outside the per-sample loop; smooth every parameter.
- MANDATORY report sections: explicit processBlock audit (allocation/locks/IO/logging/bounded work), numeric spot-checks vs the proto's numbers, clean build (zero warnings on our sources), pluginval strictness 5 SUCCESS.
- No commit — the owner reviews and commits.

## Cross-cutting rules
- Each stage's report feeds the next stage's prompt verbatim where formulas are involved.
- UI-facing math must share a single-source recipe header with the kernel (see src/dsp/BandResponse.h pattern) so drawn curves cannot drift from the actual filter.
- If two research agents disagree on a convention, resolve numerically in the proto, not by argument.
