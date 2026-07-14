# Compressor Reference Guide — Concepts, and Where the Differentiation Lives

## 0. First: one reference is missing from your list

The actual README's reference list starts with:

> **Dimitrios Giannoulis, Michael Massberg, Joshua D. Reiss. *Digital Dynamic Range Compressor Design — A Tutorial and Analysis*. JAES (2012).**

This is the **body** of the compressor. The other four (Vicanek x2, Redmon, Makima) are **supporting infrastructure**. Roughly: one paper is 80% of the compressor, the other four are 20%. Understand this hierarchy before anything else.

## 1. Signal flow — where each reference sits

```
                 +------------- sidechain (detector) path -------------+
 IN --+--------> | SC filter -> |x| (or x^2) -> to dB -> Gain Computer  |
      |          |      ^                                    |          |
      |          |  [Vicanek x2]                       [Giannoulis]     |
      |          |  [Redmon]                           [Makima: curve]  |
      |          |                                            v         |
      |          |                                  Ballistics (A/R) -+ |
      |          |                                    [Giannoulis]    | |
      |          +-------------------------------------------------+--+
      |                                                            |
      +--> [Lookahead Delay] -----------------> x <--- 10^(g/20) --+
                                                |
                                               OUT
```

| Reference | Where it sits | Weight |
|---|---|---|
| **Giannoulis et al. (2012)** | Gain computer + ballistics + overall topology | Dominant |
| Vicanek 2016 / 2019 | Sidechain EQ biquad / one-pole shelf coefficients | Medium |
| Redmon (Cascading) | Sidechain HP/LP slopes (12/24/48 dB/oct) | Small |
| Makima | Transfer curve and GUI curve interpolation | Small |

---

## 2. Giannoulis, Massberg & Reiss (2012) — *Digital DRC Design*

The de facto textbook for compressor implementation. If you skip this, you will rewrite your compressor.

### 2.1 Topology taxonomy — the paper's backbone

Compressor design decomposes into three binary choices:

1. **Feedforward vs Feedback**
   - FF: gain derived from the input. Predictable, allows lookahead, the modern digital standard.
   - FB: gain derived from the output. The analog approach (1176, LA-2A). Cannot do infinite ratio, cannot do lookahead, but the gain reduction is inherently "self-limiting" and softer.

2. **Linear domain vs Log domain**
   - Do you compute and smooth the gain in linear amplitude, or in **dB (log)**?
   - **A central conclusion: use the log domain.** Time constants become independent of signal level, the ratio becomes a straight line in dB, and "10 ms attack" actually means 10 ms regardless of how loud the input is. In the linear domain the effective time constants wobble with program material.

3. **Level detection placement**: smooth **before** the gain computer, or **after** it (i.e. smooth the gain signal itself)?
   - Smoothing the gain signal (after) generally behaves better.

### 2.2 The three peak detectors — the most important distinction

How you implement attack/release *is* the sound.

- **Branching**: `if (x > y) use attack coeff else use release coeff`. Simplest. Problem: switching from release back to attack produces a **discontinuity (kink)**, and the release never truly completes.
- **Decoupled**: separate the release one-pole and the attack one-pole into **two cascaded stages**: `y1 = max(x, release(y1))` then `y = attack(y1)`. Much smoother.
- **Smooth decoupled** <- **the paper's recommendation**: the decoupled detector with the `max()` replaced by a smooth operation. Closest to analog circuit behavior, no kink.

> **Recommended baseline: Feedforward + Log domain + Smooth decoupled peak detector.** Start here.

### 2.3 Time-constant coefficients

The one-pole smoother coefficient:

```
alpha = exp(-1 / (tau * fs))
```

This is **not** a bilinear transform — it is an exact (impulse-invariant) pole mapping. Note that this is the same idea Vicanek applies to filters: map the pole exactly with `exp()`, solve the numerator separately. Your attack/release is already doing "matched" filter design.

- Watch out: whether `tau` is defined as "time to reach 63.2%" or "10% -> 90% rise time" changes the coefficient. The paper is explicit about this. If you want your attack times to be comparable to commercial compressors, you must match the definition.

### 2.4 Gain computer and soft knee

Static transfer function in dB:

```
          | x                                     , x - T < -W/2
y_G(x) =  | x + (1/R - 1)(x - T + W/2)^2 / (2W)   , |x - T| <= W/2   <- quadratic knee
          | T + (x - T)/R                         , x - T > W/2
```

- The knee region is a **quadratic polynomial** whose **first derivative is continuous** at both knee boundaries. Without this C1 continuity you get chatter around the threshold.
- Gain = `y_G(x) - x`. Smooth that, then apply `10^(g/20)`.

### 2.5 What to take away

1. The **design space map**: FF/FB x linear/log x three detectors. You should be able to state where your compressor sits.
2. **Compute and smooth gain in the log domain**, and why.
3. Derivation of the smooth decoupled peak detector.
4. `alpha = exp(-1/(tau*fs))` and the time-constant definition trap.
5. The C1-continuous soft knee formula.

---

## 3. Martin Vicanek — *Matched Second Order Digital Filters* (2016)

### Core idea

The RBJ Cookbook biquad is built on the **bilinear transform (BLT)**, which warps the frequency axis and **compresses the response near Nyquist** (cramping).

Vicanek's fix:

1. **Poles**: map exactly via impulse invariance (`a1 = -2 e^{-alpha} cos(beta)`, `a2 = e^{-2 alpha}`). Q and decay are preserved exactly.
2. **Zeros**: solve separately by **magnitude matching**. The key trick is the substitution `phi = sin^2(w/2)`. Since `|1 - e^{-jw}|^2 = 4 sin^2(w/2)`, the squared magnitude `|H(e^{jw})|^2` becomes a **rational function in phi**, and matching conditions at DC / Nyquist / the corner frequency reduce to a linear system with a closed-form solution.
3. Phase is sacrificed (hence matched *magnitude*).

### Role in a compressor

**Sidechain EQ.** Specifically:

- A **sidechain high-pass** so that low frequencies don't over-trigger the compressor.
- **Sidechain peak/shelf** filters so only a chosen band drives the detector (de-essing, frequency-dependent response).
- Filtering of an external sidechain input.

### What matters here

The detector filter is never heard directly. So why does accuracy matter?

1. You draw the sidechain response curve in the GUI. What you draw must be what actually happens.
2. With a BLT-based sidechain high-pass at, say, 8 kHz, the response is visibly distorted near Nyquist, so the detector responds to a **different band than the user intended**. That difference shows up directly as a different amount of gain reduction — audible.

Note that **phase accuracy is far less important here** than in an EQ, since the detector only uses magnitude. Vicanek discarding phase is harmless in this context.

---

## 4. Martin Vicanek — *Matched One-Pole Digital Shelving Filters* (2019)

### Core idea

The first-order version. One pole, one zero.

- The pole is matched exactly.
- The zero is solved in closed form by matching three anchors: **DC gain, Nyquist gain, and the midpoint frequency** (where the gain equals the geometric mean).
- The gain `G` enters as `sqrt(G)`. Conceptually: a shelf is a low/high-pass shifted along the frequency axis by an amount set by the gain.

### Role in a compressor

- Sidechain **tilt/shelf** shaping ("make it respond more to highs").
- One-poles are **extremely cheap**, so they cost nothing in the detector path. In the detector, CPU and response accuracy matter more than audio-grade quality.
- Conceptually important: **the attack/release smoother is itself a one-pole.** `alpha = exp(-1/(tau*fs))` is exactly the "exact pole mapping" of this paper. After reading it, you start seeing ballistics as filter design, which is the right mental model.

---

## 5. Nigel Redmon — *Cascading Filters* (2016)

### Core idea

- **Stacking two Butterworth 2nd-order sections (Q = 0.7071) does not give a 4th-order Butterworth.** You get a 4th-order Linkwitz-Riley: -6 dB at the corner (not -3 dB), with a sagging passband.
- A true Nth-order Butterworth requires each biquad section to have a **different Q** (poles evenly distributed on the left half of the unit circle in the s-plane):
  - `Q_k = 1 / (2 cos(theta_k))`
  - 4th order: **0.5412, 1.3066**. 6th order: **0.5177, 0.7071, 1.9319**. Odd orders add a one-pole section.

### Role in a compressor

- The Q table for **sidechain HP/LP slope selection** (12/24/36/48 dB/oct).

### The hidden, more important implication

Redmon's underlying principle — **cascading changes the effective characteristic** — applies directly to **ballistics**.

- Cascading N one-pole smoothers (which converges toward a Gaussian) gives a **much smoother attack/release curve**. This is a large part of the "relaxed" feel of analog compressors.
- But **naively cascading lengthens the effective attack time.** To keep the overall time constant at `tau` with N stages, each stage needs roughly `tau/N` (or a proper correction if you define attack by 10-90% rise time).
- Skip this correction and you ship a compressor whose label says "5 ms attack" while it actually takes 15 ms. Extremely common bug. **Applying Redmon's lesson to the ballistics, not just the sidechain filter, is the real value of this reference.**

---

## 6. Cleve Moler — *Makima Piecewise Cubic Interpolation* (2019)

### Core idea

- **Akima interpolation**: a local piecewise-cubic Hermite scheme where each knot's slope is a weighted combination of neighboring divided differences. Crucially, it **does not overshoot or ring** (the decisive difference from a cubic spline).
- Original Akima's weakness: when consecutive values are equal (flat regions), the weight denominator goes to zero, producing artifacts.
- **Makima (modified Akima)**: reworked weights that handle this degenerate case. This is MATLAB's `makima`.

### Role in a compressor — this is a safety requirement, not cosmetics

1. **The transfer curve.** If you offer a multi-point editable compression curve, you must interpolate between the user's points.
   - **Use a spline and the curve will overshoot.** An overshooting transfer curve has regions where the **local slope is negative or greater than 1**.
   - Slope > 1 means that region is an **expander**, not a compressor.
   - Slope < 0 means louder input produces quieter output. That is a broken, oscillation-prone system.
   - So **monotonicity of the transfer curve is a correctness constraint**, not a matter of taste.
2. Knee and curve visualization; gain-reduction meters and histograms.
3. Interpolation between automation points.

### What to take away

- Recognize the class of problems where **no-overshoot and monotonicity matter more than smoothness**. Audio parameters, especially gain mappings, are almost always in this class.
- This is the same motivation behind Giannoulis's quadratic soft knee: the gain curve must be smooth *and* monotonic.

---

# Part II — Where Differentiation Actually Comes From

## 7. The uncomfortable truth about the reference list

> **Giannoulis describes the *ideal* compressor. Every characteristic people love about compressors is a *deviation* from ideal.**

Implement these five references cleanly and you get a transparent, technically correct compressor — and no identity. Differentiation comes from **which deviations you deliberately design in, and how you make them controllable**.

### 7.1 The six slots

Decompose a compressor into slots. A clean implementation picks the left column for all six. Differentiation means picking the right column for **one or two** slots — not all of them. Changing everything just produces an incoherent plugin.

| Slot | Clean | Character-producing choice |
|---|---|---|
| 1. Topology | Feedforward | **Feedback** |
| 2. Release | Fixed tau | **Program-dependent release** |
| 3. Gain signal | Applied as-is | **Process the gain signal itself** |
| 4. Nonlinearity | None | **Saturation coupled to gain reduction** |
| 5. Attack curve | Exponential (one-pole) | **S-curve / quasi-linear** |
| 6. Detection target | Broadband peak | **Split transient vs sustain** |

---

## 8. What "punch" actually is

It is not vibes. It is two measurable quantities.

### (a) The shape of the gain envelope in the first ~5 ms

Feed a kick drum in and **plot the gain reduction signal**. Punch is entirely about how the gain moves right after the transient arrives.

- A one-pole exponential attack moves **fastest at t=0** (the derivative of `1 - e^{-t/tau}` is maximal at zero). So short attack times chew directly into the front of the transient.
- An **S-curve attack** (2-3 cascaded one-poles) has **zero initial slope**. The gain does not move during the first instant of the transient. You keep a short attack time and still preserve the attack sound.
- This is a large part of why analog compressors "feel punchier" — real circuits are not pure one-poles.

### (b) Crest factor delta

Measure `20*log10(peak/RMS)` at the input and the output. Good punch means: **raise RMS while destroying as little crest factor as possible.**

Build this as a measurement rig and you can tune your character by numbers instead of by feel. Most plugin developers do not do this. It is one of the highest-leverage things you can build early.

---

## 9. What "the saturation sounds great" actually means

Saturation itself is trivial (`tanh`). Making it *feel* good is entirely about **where you put it**.

Three positions, three completely different results:

1. **Input stage** (transformer / preamp emulation) — **before the detector**. The saturation shaves the transient first, so the compressor reacts to an already-softened signal. Creates interaction between drive and compression.
2. **The gain element itself — this is the one that matters.** Real VCA/FET nonlinearity **increases with the amount of gain reduction**.
   - Structure: `y = x * g * (1 + k * (1 - g) * nonlin(x))`. At 0 dB GR there is no distortion; at 10 dB GR the harmonics come up hard.
   - **This is what "it reacts" means.** Hit it hard and it gets dirty; play softly and it stays clean. A fixed saturation stage can never do this.
   - Bolting a `tanh` onto the output instead gives you the "there's a saturate button but it's boring" plugin. This is where most free plugins fail.
3. **Output stage** — just tone shaping. The least interesting option.

**Oversample the nonlinear section** (4x minimum, only around the saturator). Without it, aliasing sprays metallic garbage across the top end.

---

## 10. The most underrated insight: the gain signal is an audio signal

`out[n] = in[n] * gain[n]` is **amplitude modulation** — a convolution in the frequency domain. Therefore:

- **Low-frequency ripple in the gain signal is harmonic distortion.** Put a fast attack on a 60 Hz bass note and the gain will oscillate at the waveform period (120 Hz), producing intermodulation products. **This is precisely the mechanism behind "the compressor adds warmth to the bass."** It is not a bug — it is the sound.
- Most plugins *remove* this with a detector high-pass. But **whether to remove it, and how much to leave in, can be a user-facing character control.**
- If the gain signal is broadband, the multiply aliases. Oversampling **only the gain path** (not the audio path) fixes this cheaply.

---

## 11. Three concrete proposals

### Proposal A: "Gain Path Designer" — make the gain signal a first-class citizen (recommended)

Reframe the compressor as **a synthesizer that generates a gain signal**, and expose that signal's path to the user.

- A dedicated **filter on the gain signal** (keep or remove the low-frequency ripple -> a direct distortion-amount control).
- A dedicated **saturator/shaper on the gain signal** (bend the gain curve to reshape the attack/release contour directly).
- **Overlay the gain signal on the waveform** in real time.
- Use the Makima curve editor to let the user **draw the attack/release curve shape itself**, not just set a single tau.

Why this is good: nobody is doing it, it is conceptually honest, implementation difficulty is moderate, and it demos beautifully. For an open-source plugin competing against commercial ones, conceptual novelty is a real weapon.

### Proposal B: Program-dependent release (best effort-to-payoff ratio)

The smallest amount of code that buys the largest "wait, this sounds good."

- Make the release time constant a **function of the gain-reduction history**. Short, hard GR -> fast release. Sustained GR -> slow release.
- Implementation: run a fast-release one-pole and a slow-release one-pole in parallel, and **blend them based on accumulated GR state**. This is physically what the opto cell in an LA-2A does.
- About 30 lines. The effect is dramatic.

### Proposal C: Transient / sustain split detection

Run two detectors — one fast, one slow. Their difference is the **transient component**.

- Give the user **separate ratios** for the transient component and the sustain component.
- "Preserve the transient, only squash the body" (or the reverse) becomes a single knob. This **promotes punch from a vibe into an explicit parameter**.

---

## 12. The honest part

The real #1 commercial differentiator for compressor plugins is, bluntly, **the UI and the metering**. Pro-C 2 won on its display — gain reduction drawn over the waveform in real time — not on its algorithm. Users do not judge punch by ear alone; **they see it and then believe it.**

Since the codebase already has strong visualization infrastructure (JUCE, kfr), **Proposal A is the one option that captures a technical differentiator and a visual differentiator with the same work.**

## 13. Where to start

Before writing any DSP for the "kick," build the measurement rig:

**An automated harness that, for a set of kick/snare/bass/full-mix samples, plots the gain envelope and reports the crest-factor delta.**

Without it, "our sound" is an assertion of taste. With it, tuning becomes engineering.

---

# Suggested Learning / Build Order

**Stage 1 — Read Giannoulis, build a minimal compressor (3-5 days)**
No sidechain EQ, no GUI, no lookahead:
`|x| -> dB -> gain computer (hard knee) -> branching detector -> gain -> multiply`

- Feed step and sine inputs and **plot the gain reduction signal itself**. Look at the gain curve before you listen to anything.
- Then swap branching -> decoupled -> smooth decoupled and **overlay the same plot**. You will see the kink disappear.
- Then build a linear-domain version and **measure whether the attack time actually changes with input level.** That experiment is how you feel why the log domain is necessary.

**Stage 2 — Soft knee and accurate time constants (1-2 days)**
Implement the knee and plot the transfer curve. Verify C1 continuity at the knee boundaries by numerical differentiation. Measure the actual 10-90% rise time and confirm it matches the displayed attack value.

**Stage 3 — Sidechain EQ (Vicanek + Redmon) (2-3 days)**
Build the SC high-pass with an RBJ biquad first, then swap in matched coefficients and **plot the difference above 8 kHz**. Add 24/48 dB/oct slopes using Redmon's Q table.

**Stage 4 — Curves and UI (Makima) (1 day)**
If you build a transfer-curve editor, use Makima, and **assert that the interpolated slope always stays within [0, 1]**.

**Stage 5 — The differentiator**
Only now pick one or two slots from Section 7.1. Start with Proposal B (cheapest, immediate payoff), then Proposal A (the real identity).

---

# What This Reference List Does Not Cover

You will have to learn these from source code and experimentation, not from these papers:

- **Lookahead**: looking ahead at the gain signal to smooth it (simple delay + max-filter, or low-passing the gain signal itself). Requires correct PDC latency reporting.
- **Gain-signal aliasing**: gain multiplication is amplitude modulation. With very fast attacks, the **gain signal's harmonics alias**. Oversampling only the gain path (not the audio) is the cheap fix.
- **RMS vs Peak vs True-Peak** detection.
- **Stereo linking** (max, sum, mid/side, partial link).
- **Program-dependent / auto release** (blending multiple release stages) — the actual mechanism behind the "magic" of famous analog units.
- **Upward compression / expansion / gating.**

These six are outside the scope of the Giannoulis paper but are exactly what separates a competent compressor from a commercial-grade one.
