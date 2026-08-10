"""soothe3 BLACK-BOX MEASUREMENT KIT — signal generator (Stage 0).

Generates a set of probe WAVs whose bounced-through-soothe3 counterparts let us
INFER the plugin's internal design without any source access. The mapping from
each probe to the inferred quantity (the "왜 이 측정인가"):

  noise_white        white noise in/out delta -> band resolution + envelope
                     smoothing width (how finely it splits the spectrum).
  sine_staircase     one sine, level stepped -> threshold CHARACTER: if the
                     steady-state attenuation is the same at every input level,
                     the threshold is RELATIVE (soft mode); if it kicks in only
                     above a fixed level, it is ABSOLUTE (hard mode).
  resonance_in_noise a resonance rising out of a noise floor -> the core
                     mechanism: attenuation as a function of how far the tone
                     EXCEEDS the local spectral envelope (the "money" transfer).
  burst_3k           a tone gated on/off -> gain-reduction attack/release time
                     constants (how fast it clamps and recovers).
  pair_sweep         two tones at shrinking spacing -> frequency-kernel width
                     (how wide a region one detected peak influences), the
                     physical meaning of the Detail control.
  passthrough_ref    noise+staircase, for latency alignment / null checks.

All signals: fs=44100, STEREO (mono duplicated to both channels, so L==R and any
channel-linking in the plugin is exercised identically), 32-bit float WAV.

HEADROOM: every file peaks at or below -6 dBFS. The single loudest sample in the
kit is the top staircase step, a -6 dBFS sine (peak == 0.501). Everything else is
quieter. So there is >= 6 dB of headroom everywhere and no bounce can clip even
if the plugin adds a little gain. Levels are stated per-signal below.

The generator also writes manifest.json alongside the WAVs. That manifest is the
SINGLE SOURCE OF TRUTH for segment timing — the analyzer reads it to know where
each staircase/pair/burst step starts and stops, so the two scripts can never
drift out of sync.
"""
import json
import os

import numpy as np
from scipy.io import wavfile

FS = 44100
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = os.path.join(REPO_ROOT, "proto", "out", "soothe3_kit")
IN_DIR = os.path.join(KIT, "in")

# Peak safety ceiling used by the asserts. Nothing we build should come near it.
PEAK_CEIL = 0.9  # ~ -0.9 dBFS

_RNG = np.random.default_rng(20260721)  # fixed seed -> byte-reproducible noise


# ----------------------------------------------------------------------------
# level helpers
# ----------------------------------------------------------------------------
def dbfs_to_amp(db):
    """dBFS -> linear peak amplitude (0 dBFS == 1.0)."""
    return 10.0 ** (db / 20.0)


def sine_amp_for_rms_dbfs(rms_dbfs):
    """Peak amplitude of a sine whose RMS is rms_dbfs. A sine of peak a has
    RMS a/sqrt(2), i.e. RMS is 3.01 dB below peak; invert that here."""
    return dbfs_to_amp(rms_dbfs) * np.sqrt(2.0)


def _t(n_samples):
    return np.arange(n_samples) / FS


def sine(freq, dur, amp, phase=0.0):
    n = int(round(dur * FS))
    return amp * np.sin(2.0 * np.pi * freq * _t(n) + phase)


def _cos_fade(x, fade_s):
    """Raised-cosine fade in/out over fade_s at each end (click suppression)."""
    n_fade = int(round(fade_s * FS))
    if n_fade <= 0 or 2 * n_fade >= len(x):
        return x
    ramp = 0.5 * (1.0 - np.cos(np.pi * np.arange(n_fade) / n_fade))
    x = x.copy()
    x[:n_fade] *= ramp
    x[-n_fade:] *= ramp[::-1]
    return x


def white_noise(dur, rms_dbfs):
    n = int(round(dur * FS))
    x = _RNG.standard_normal(n)
    x *= dbfs_to_amp(rms_dbfs) / np.sqrt(np.mean(x**2))
    return x


# ----------------------------------------------------------------------------
# segment-timing spec (mirrored into manifest.json)
# ----------------------------------------------------------------------------
STEP_DUR = 1.5     # s per staircase / resonance step
STEP_FADE = 0.02   # s raised-cosine between steps
STEP_GUARD = 0.25  # s trimmed off each end of a step before measuring steady state

STAIRCASE_LEVELS_DBFS = list(range(-60, -5, 6))          # -60..-6, 6 dB steps (10)
RES_NOISE_RMS_DBFS = -36.0
RES_SINE_REL_DB = [-12, -6, 0, 6, 12, 18, 24]            # tone level relative to noise (7)
RES_SINE_FREQ = 3000.0

WHITE_RMS_DBFS = -20.0
WHITE_DUR = 15.0

BURST_FREQ = 3000.0
BURST_RMS_DBFS = -12.0
BURST_ON = 1.0
BURST_OFF = 1.0
BURST_CYCLES = 6
BURST_EDGE = 0.001   # 1 ms tone edges — fast vs plugin GR, slow enough to not splatter
BURST_BED_RMS_DBFS = -50.0  # constant low noise bed under the whole file. WHY: the
# GR RELEASE cannot be seen on pure silence (no carrier to imprint the gain onto).
# A quiet broadband bed lets the 3 kHz region's gain-recovery show up during the
# OFF gaps, so release is measurable; the bed is 38 dB under the tone and far below
# any threshold, so it neither triggers the plugin nor disturbs the attack read.

PAIR_FC = 3000.0
PAIR_SPACINGS_OCT = [2.0, 1.0, 0.5, 1.0 / 3.0, 1.0 / 6.0, 1.0 / 12.0]
PAIR_STEP_DUR = 2.0
PAIR_TONE_RMS_DBFS = -18.0

# notch_probe: ONE loud sine on a broadband bed -> direct kernel-WIDTH read.
NOTCH_FREQ = 3000.0
NOTCH_TONE_RMS_DBFS = -18.0   # loud tone, well above the bed -> a clear resonance
NOTCH_BED_RMS_DBFS = -36.0    # broadband bed the notch is carved into
NOTCH_DUR = 6.0


# ----------------------------------------------------------------------------
# signal builders — each returns (mono float64, segments-list-or-None)
# ----------------------------------------------------------------------------
def build_noise_white():
    return white_noise(WHITE_DUR, WHITE_RMS_DBFS), None


def _staircase(freq, levels_dbfs):
    """A single sine held at each level in `levels_dbfs` for STEP_DUR, with a
    short cross-fade between steps. Returns (signal, segments) where each segment
    records the level and the [start,end] of its steady (post-guard) window."""
    parts, segs, t0 = [], [], 0.0
    for db in levels_dbfs:
        seg = sine(freq, STEP_DUR, dbfs_to_amp(db))
        seg = _cos_fade(seg, STEP_FADE)
        parts.append(seg)
        segs.append({
            "label_db": db,
            "t_start": round(t0, 6),
            "t_end": round(t0 + STEP_DUR, 6),
            "steady_start": round(t0 + STEP_FADE + STEP_GUARD, 6),
            "steady_end": round(t0 + STEP_DUR - STEP_GUARD, 6),
        })
        t0 += STEP_DUR
    return np.concatenate(parts), segs


def build_sine_staircase_1k():
    return _staircase(1000.0, STAIRCASE_LEVELS_DBFS)


def build_sine_staircase_4k():
    return _staircase(4000.0, STAIRCASE_LEVELS_DBFS)


def build_resonance_in_noise():
    """Fixed white-noise floor + a 3 kHz sine stepped from below the floor to
    +24 dB above it. The exceed-over-floor axis is the relative-threshold probe."""
    noise_amp = dbfs_to_amp(RES_NOISE_RMS_DBFS)  # target RMS
    parts, segs, t0 = [], [], 0.0
    for rel in RES_SINE_REL_DB:
        n = int(round(STEP_DUR * FS))
        floor = _RNG.standard_normal(n)
        floor *= noise_amp / np.sqrt(np.mean(floor**2))
        tone_rms_dbfs = RES_NOISE_RMS_DBFS + rel
        tone = sine(RES_SINE_FREQ, STEP_DUR, sine_amp_for_rms_dbfs(tone_rms_dbfs))
        tone = _cos_fade(tone, STEP_FADE)
        parts.append(floor + tone)
        segs.append({
            "rel_db": rel,
            "tone_rms_dbfs": round(tone_rms_dbfs, 3),
            "noise_rms_dbfs": RES_NOISE_RMS_DBFS,
            "freq": RES_SINE_FREQ,
            "t_start": round(t0, 6),
            "t_end": round(t0 + STEP_DUR, 6),
            "steady_start": round(t0 + STEP_FADE + STEP_GUARD, 6),
            "steady_end": round(t0 + STEP_DUR - STEP_GUARD, 6),
        })
        t0 += STEP_DUR
    return np.concatenate(parts), segs


def build_burst_3k():
    """3 kHz tone, BURST_ON s on / BURST_OFF s off, repeated. Tone edges are a
    1 ms raised cosine — fast relative to any plausible GR time constant so the
    measured attack/release reflect the plugin, not our fade — but slow enough
    to avoid spectral splatter that would smear the 3 kHz bin."""
    on = _cos_fade(sine(BURST_FREQ, BURST_ON, dbfs_to_amp(BURST_RMS_DBFS) * np.sqrt(2.0)),
                   BURST_EDGE)
    off = np.zeros(int(round(BURST_OFF * FS)))
    parts, segs, t0 = [], [], 0.0
    for _ in range(BURST_CYCLES):
        parts.append(on)
        segs.append({"kind": "on", "t_start": round(t0, 6), "t_end": round(t0 + BURST_ON, 6)})
        t0 += BURST_ON
        parts.append(off)
        segs.append({"kind": "off", "t_start": round(t0, 6), "t_end": round(t0 + BURST_OFF, 6)})
        t0 += BURST_OFF
    tone = np.concatenate(parts)
    bed = white_noise(len(tone) / FS, BURST_BED_RMS_DBFS)[:len(tone)]
    return tone + bed, {"freq": BURST_FREQ, "bed_rms_dbfs": BURST_BED_RMS_DBFS, "segments": segs}


def build_pair_sweep():
    """Two equal-level sines centred (geometric mean) on 3 kHz, their spacing
    stepped 2 -> 1/12 octave. As they close, a frequency kernel that is wider
    than their spacing can no longer treat them as two separate peaks; the
    spacing at which their mutual influence appears IS the kernel width."""
    tone_amp = sine_amp_for_rms_dbfs(PAIR_TONE_RMS_DBFS)
    parts, segs, t0 = [], [], 0.0
    for oct_sp in PAIR_SPACINGS_OCT:
        f_lo = PAIR_FC * 2.0 ** (-oct_sp / 2.0)
        f_hi = PAIR_FC * 2.0 ** (+oct_sp / 2.0)
        seg = sine(f_lo, PAIR_STEP_DUR, tone_amp) + sine(f_hi, PAIR_STEP_DUR, tone_amp)
        seg = _cos_fade(seg, STEP_FADE)
        parts.append(seg)
        segs.append({
            "spacing_oct": round(oct_sp, 6),
            "f_lo": round(f_lo, 4),
            "f_hi": round(f_hi, 4),
            "t_start": round(t0, 6),
            "t_end": round(t0 + PAIR_STEP_DUR, 6),
            "steady_start": round(t0 + STEP_FADE + STEP_GUARD, 6),
            "steady_end": round(t0 + PAIR_STEP_DUR - STEP_GUARD, 6),
        })
        t0 += PAIR_STEP_DUR
    return np.concatenate(parts), segs


def build_notch_probe():
    """A single loud 3 kHz sine sitting on a broadband white-noise bed. soothe3
    detects the tone as a resonance and carves a NOTCH around it; because the bed
    supplies probe energy at EVERY frequency, the width of the reduction the notch
    imprints on the bed (attenuation-vs-frequency around 3 kHz) is a DIRECT read of
    the frequency-kernel width.

    WHY a new probe: pair_sweep has energy only AT its two tones, so the gap
    between them is unmeasurable and per-tone attenuation confounds kernel width
    with processing depth and with absolute frequency (the tones move 1500..6000 Hz
    across the sweep). This probe removes all three confounds — fixed frequency,
    broadband bed to read the shoulders, one tone so depth is just the notch floor.
    Bounce ONCE PER Detail (0/50/100) to map Detail -> kernel width in octaves."""
    n = int(round(NOTCH_DUR * FS))
    bed = _RNG.standard_normal(n)
    bed *= dbfs_to_amp(NOTCH_BED_RMS_DBFS) / np.sqrt(np.mean(bed**2))
    tone = sine(NOTCH_FREQ, NOTCH_DUR, sine_amp_for_rms_dbfs(NOTCH_TONE_RMS_DBFS))
    tone = _cos_fade(tone, STEP_FADE)
    return bed + tone, {"freq": NOTCH_FREQ,
                        "tone_rms_dbfs": NOTCH_TONE_RMS_DBFS,
                        "bed_rms_dbfs": NOTCH_BED_RMS_DBFS}


def build_passthrough_ref():
    """5 s white noise + the 1 kHz staircase, concatenated (~20 s), for
    latency / null alignment. Broadband + tonal content gives cross-correlation
    a sharp peak."""
    noise = white_noise(5.0, WHITE_RMS_DBFS)
    stair, _ = _staircase(1000.0, STAIRCASE_LEVELS_DBFS)
    return np.concatenate([noise, stair]), None


BUILDERS = {
    "noise_white": (build_noise_white, "white noise in/out delta -> band resolution + envelope smoothing width"),
    "sine_staircase_1k": (build_sine_staircase_1k, "1 kHz level staircase -> threshold character (relative vs absolute)"),
    "sine_staircase_4k": (build_sine_staircase_4k, "4 kHz level staircase -> threshold character at a second frequency"),
    "resonance_in_noise": (build_resonance_in_noise, "3 kHz tone rising out of a noise floor -> exceed->reduction transfer (core mechanism)"),
    "burst_3k": (build_burst_3k, "gated 3 kHz tone -> GR attack/release time constants"),
    "pair_sweep": (build_pair_sweep, "closing tone pair -> frequency-kernel width vs Detail"),
    "passthrough_ref": (build_passthrough_ref, "noise+staircase -> latency alignment / null check"),
    "notch_probe": (build_notch_probe, "one loud sine on a broadband bed -> DIRECT frequency-kernel width vs Detail"),
}


# ----------------------------------------------------------------------------
# write + verify
# ----------------------------------------------------------------------------
def _to_stereo_float32(mono):
    x = np.asarray(mono, dtype=np.float64)
    return np.column_stack([x, x]).astype(np.float32)


def _verify(name, mono):
    x = np.asarray(mono, dtype=np.float64)
    assert not np.any(np.isnan(x)), f"{name}: NaN in signal"
    assert not np.any(np.isinf(x)), f"{name}: Inf in signal"
    peak = float(np.max(np.abs(x)))
    assert peak < PEAK_CEIL, f"{name}: peak {peak:.4f} >= ceiling {PEAK_CEIL}"
    assert len(x) > 0, f"{name}: empty"
    return peak


def main():
    os.makedirs(IN_DIR, exist_ok=True)
    manifest = {"fs": FS, "format": "32-bit float stereo (mono duplicated)",
                "headroom_note": "loudest sample is the -6 dBFS top staircase step; >=6 dB headroom everywhere",
                "step_dur_s": STEP_DUR, "step_fade_s": STEP_FADE, "step_guard_s": STEP_GUARD,
                "files": {}}
    rows = []
    for name, (builder, purpose) in BUILDERS.items():
        mono, segs = builder()
        peak = _verify(name, mono)
        dur = len(mono) / FS
        path = os.path.join(IN_DIR, name + ".wav")
        wavfile.write(path, FS, _to_stereo_float32(mono))
        # confirm what actually landed on disk
        rfs, rdata = wavfile.read(path)
        assert rfs == FS and rdata.shape[0] == len(mono) and rdata.shape[1] == 2, \
            f"{name}: readback mismatch"
        manifest["files"][name] = {
            "duration_s": round(dur, 4),
            "peak_dbfs": round(20 * np.log10(peak), 2),
            "purpose": purpose,
            "segments": segs,
        }
        rows.append((name, dur, peak, purpose))

    with open(os.path.join(IN_DIR, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)

    print("=" * 88)
    print("soothe3 measurement kit — generated WAVs")
    print(f"  fs={FS}  format=32-bit float stereo (mono duplicated)  dir={IN_DIR}")
    print(f"  headroom: loudest sample is the -6 dBFS top staircase step (>=6 dB everywhere)")
    print("=" * 88)
    print(f"  {'file':22s} {'dur(s)':>7s} {'peak dBFS':>10s}  purpose")
    print("-" * 88)
    for name, dur, peak, purpose in rows:
        print(f"  {name+'.wav':22s} {dur:7.2f} {20*np.log10(peak):10.2f}  {purpose}")
    print("-" * 88)
    print(f"  manifest.json written with per-file segment timing (analyzer reads it)")
    print(f"  total files: {len(rows)}  all asserts PASS (no NaN/Inf, peak < {PEAK_CEIL})")
    print("=" * 88)


if __name__ == "__main__":
    main()
