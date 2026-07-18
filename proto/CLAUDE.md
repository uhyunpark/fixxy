# proto/CLAUDE.md

Python prototyping stage: research -> numpy proto with independent numeric
verification -> only then C++. A derivation without a passing assert-based
verification does not enter C++.

## Running protos

One venv, at `proto/.venv`, pinned by `proto/requirements.txt`
(numpy/scipy/matplotlib/sounddevice). Invoke from the repo root:

```
./proto/.venv/bin/python proto/<script>.py
```

## Verification is mandatory, not optional

Every proto asserts its output against an **independent** reference — scipy.signal,
an analog prototype evaluation, or a closed-form check — never against its own
derivation. This has caught real errors before it reached C++:

- A research-stage Butterworth pole-allocation formula (cos-form instead of
  sin-form) was wrong in a way that only an even-order coincidence hid — caught by
  a scipy assert (PROJECT.md 함정).
- A published reference summary (docs/eq_reference_guide.md) had two transcription
  errors (an Ivantsov φ-function numerator typo, an inverted sigma range) —
  caught only by numeric comparison against the original paper, not by re-reading
  the summary. Research -> document -> implementation can introduce a
  transcription error at any stage; only the proto's numeric cross-check against
  the *original* source catches it.

A failed assert means fix the formula, not loosen the tolerance.

## House style

- Numbered tests (`test 1`, `test 2`, ...) printed with PASS/FAIL and the actual
  numbers, not just a boolean.
- A `SUMMARY` + `VERDICT` block at the end.
- Docstrings explain the DSP *why*, not just the *what* — these are read later as
  ground truth by the C++ port stage.
- WAVs and plots go to `proto/out/`.

## Fixture dump scripts are the bridge to C++

`dump_*.py` scripts (e.g. `dump_matched_fixtures.py`, `dump_dynamic_fixtures.py`)
regenerate the `test/*_fixtures.h` headers consumed by the C++ console tests.
**Regenerate fixtures by re-running the dump script, never by hand-copying numbers**
out of a proto's stdout — the dump script is where the proto's verified pipeline
gets pinned to what the C++ kernel actually needs to reproduce.

This isn't always a 1:1 pass-through of the source proto's numbers — see the
Ivantsov/Vicanek reconciliation note at the top of `dump_dynamic_fixtures.py`: the
dynamic-EQ proto (`dynamic_eq.py`) was written against the Vicanek matched design,
but the shipping C++ kernel uses the Ivantsov design, so the dump script
substitutes the correct bell/detector functions and realizes the detector filter
in Direct Form I to match the C++ kernel bit-for-bit, rather than shipping
Vicanek-derived fixtures for an Ivantsov kernel. Read that docstring before writing
a new dump script — it's the template for reconciling "the proto that explored the
idea" against "the design that actually shipped."
