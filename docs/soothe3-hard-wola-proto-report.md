# Additive Hard WOLA — Stage 2 report

Date: 2026-09-22. Scope: the already-verified absolute Hard knee integrated
into the exact current legacy N=1024, H=256 periodic sqrt-Hann WOLA. This is
our designed approximation. It is not full parity, an FIR-engine result, a
listening verdict, or evidence of proprietary soothe internals. Original soothe
comparison remains unavailable because the reference instance could not be
loaded reliably; it was not required for this bounded gate.

## Selected behavior

- `detectionMode`: additive choice, Soft=0 and Hard=1, default Soft. No other
  new public controls are part of this handoff.
- Soft calls the existing `SidebarChannelCore.frame_gain` unchanged. The new
  fixtures must be added without removing or changing any old Soft fixture.
- Hard uses calibrated peak amplitude `a=2*abs(X)/sum(sqrtHann)` at positive
  interior bins and half that multiplier at DC/Nyquist. It then calls the
  verified `hard_reduction`: threshold `-6-6*depth` dBFS, 6 dB quadratic knee,
  slope 0.75, and 40 dB design-target ceiling.
- Retain legacy Detail frequency-width, literal-ms attack/release, their two
  time tilts, ordinary weight/range sensitivity, existing MAX magnitude link,
  M/S routing, and focused local depth. Focus never scales detector audio.
- Legacy `tiltDetail` sensitivity is deliberately Soft-only. Product UI must
  disable it in Hard and explain this; it must not look active while inert.
- Legacy Max Cut retains its existing position after the target knee and before
  frequency smoothing. Its 24 dB value remains the Off sentinel for this
  increment. The global 40 dB bound is a Hard target bound, not a new promise
  to clamp old envelope history instantaneously.
- Exactly silent frames request zero new reduction and release existing state.
  This is required because the log floor is not a silence substitute.

Mode requests are sampled at the next completed internal H boundary. Switching
changes only the target computer and retains the existing per-bin decoupled
`s1/y1` reduction envelope. There is no reset, duplicate state, extra FFT, or
output fade. WOLA overlap provides the existing frame interpolation. The numeric
checks below support this bounded policy; they do not prove perceptual
transparency for arbitrary program material.

## Reproduction and results

Run:

```sh
./proto/.venv/bin/python proto/spectral_hard_wola.py
./proto/.venv/bin/python proto/dump_spectral_hard_wola_fixtures.py
```

Both commands rerun all assertions; the dump refuses to precede the gates.

1. Actual-window calibration PASS. FFT versus independent direct DFT maximum
   amplitude error is `1.4155343563970746e-15`. A known bin-64 0.25-peak sine
   differs by `4.0170493387781114e-06` amplitude because the real sine's
   opposite-frequency component leaks through sqrt-Hann. DC/Nyquist endpoint
   factors pass within `1e-12`. Independent time-domain sine fits recover the
   known source amplitudes within `1e-12`.
2. Off-bin behavior is recorded, not hidden with an invented peak fitter.
   Peak-bin biases at offsets 0, 0.125, 0.25, 0.5 bins are respectively
   `+0.00011359`, `-0.12709668`, `-0.51158412`, `-2.09819556` dB.
3. Soft preservation/current framing PASS across default, M/S-unlinked and
   weighted/focused/tilted cases: maximum difference `8.326672684688674e-17`.
   The existing `spectral_sidebar.run_sidebar` batch helper has a documented
   extra-N output slice; the test deliberately leaves it unchanged and uses
   its unchanged Soft cores/gain hook with corrected current-stream alignment.
   A separate SciPy STFT/ISTFT reconstruction of recorded gain decisions differs
   by at most `1.1102230246251565e-16`. Host chunk maximum difference is zero.
4. Hard absolute-level/depth behavior PASS. At on-bin source levels
   `[-60,-36,-24,-12,-6]` dBFS, measured final-audio attenuation is
   `[0,0.11206,2.53310,8.15858,12.30834]` dB. At half-bin it is
   `[0,0.02030,2.96667,6.56725,8.37164]` dB. Tested depth monotonicity excess
   is zero. These differing on/off-bin transfers expose the scalloping above.
5. Silence/material behavior PASS. Exact-zero input output is exactly zero;
   after loud history the maximum reduction state after two seconds of silence
   is `1.2142390719966403e-16` dB. Measured RMS attenuation (not a quality
   judgment) is: broadband Soft `0.556169` dB / Hard `0.00001994` dB; harmonic
   Soft `0.872226` dB / Hard `6.673173` dB.
6. Mode switching PASS against independent SciPy reconstruction, maximum audio
   error `2.7755575615628914e-16`; arbitrary host-chunk difference is zero.
   The analytic attack-pole frame-change bound has zero excess. Observed maximum
   bin reduction change is `12.7066`, `2.27322`, `0.319070` dB at attack 1,
   27, 250 ms, below respective conservative bounds `39.8069`, `7.16981`,
   `0.844295` dB. Maximum adjacent output-sample differences near switch regions
   are `0.10314`, `0.10225`, `0.07716`; these include the 3/7 kHz probe waveform
   itself and are reported as measurements, not a psychoacoustic click limit.

## Handoff artifacts and exact interface

- `proto/spectral_hard_wola.py`: formulas, streaming WOLA, independent SciPy
  reference, assertions and synthetic rendering.
- `proto/dump_spectral_hard_wola_fixtures.py`: reruns all gates, then generates
  `test/spectral_hard_wola_fixtures.h` (529,048 bytes).
- Fixture layout is channel-major. Input has two channels x 2304 float32-rounded
  source samples. Each Soft, Hard and switched expected output has two channels
  x 3328 samples, including the first 1024 reported-latency zeros. Switch requests
  occur on internal input boundaries 1280 (Hard) and 1792 (Soft). Sample rate is
  48 kHz; analysis-window sum is pinned in the header.
- Synthetic, unclipped float WAVs are
  `proto/out/spectral_hard_wola_{dry,soft,hard}.wav`, 48 kHz, two seconds.

The Stage 3 port must reproduce this new Soft fixture while all existing Soft
fixtures continue passing. It must also independently compare the new Hard and
switch outputs under arbitrary host blocks. No C++ production/schema edit is in
this Stage 2 work.

## Evidence limits

The SciPy reference independently verifies WOLA reconstruction for the gain maps,
not the detector formula itself; the exact Hard knee was independently verified
earlier in `spectral_parity.py`/`spectral_control_fixtures.h`. The legacy Soft
comparison necessarily reuses the frozen Soft core but uses a separately coded
streaming renderer and is reinforced by the SciPy audio check. No controlled
reference-product audio, broad listening panel, exhaustive automation search,
high-rate gate, new-model Detail, six-tilt behavior, sidechain, FIR transition or
complete callback CPU result is claimed here.
