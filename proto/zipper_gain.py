"""Demonstrate zipper noise: hard (stepped) gain changes vs. smoothed gain changes."""
import numpy as np
from scipy.io import wavfile
import matplotlib.pyplot as plt
import os

sr = 44100
dur = 4.0
n = int(sr * dur)
t = np.arange(n) / sr

# Dry signal: a plain tone. Any gain artifact should show up as extra energy/clicks on top of it.
# Phase offset avoids the coincidence that 440 Hz * 0.2 s = 88 whole cycles, which would put
# every gain-change boundary exactly on a zero crossing and mask the click.
sig = 0.5 * np.sin(2 * np.pi * 440 * t + 0.7)

# Gain automation: flip between -12 dB and 0 dB every 0.2 s.
# This is a deliberately harsh test case -- big, frequent jumps make the artifact obvious.
seg_len = int(0.2 * sr)
n_segs = int(np.ceil(n / seg_len))
db_seq = np.array([-12.0 if i % 2 == 0 else 0.0 for i in range(n_segs)])
target_seq = 10 ** (db_seq / 20)

# Per-sample target gain (piecewise constant, changes only at segment boundaries).
target = np.repeat(target_seq, seg_len)[:n]

# Version A: hard gain -- literally multiply by the piecewise-constant target.
# The discontinuity at each boundary is a step in amplitude, which is a broadband
# click (a step function has energy at all frequencies).
hard = sig * target

# Version B: smoothed gain -- one-pole lowpass chasing the target, ~20 ms time constant.
# This turns each step into an exponential ramp, so the derivative of gain stays
# bounded and no broadband click is injected.
tau = 0.020
k = 1 - np.exp(-1 / (tau * sr))
g = np.empty(n)
cur = target[0]
for i in range(n):
    cur += k * (target[i] - cur)
    g[i] = cur
smooth = sig * g

# Write WAVs as 16-bit PCM.
os.makedirs("proto/out", exist_ok=True)
wavfile.write("proto/out/zipper_hard.wav", sr, (hard * 32767).astype(np.int16))
wavfile.write("proto/out/zipper_smooth.wav", sr, (smooth * 32767).astype(np.int16))

# --- Figure: waveform zoom around one transition + spectrograms ---
fig, axs = plt.subplots(2, 2, figsize=(11, 7))

# Zoom on the transition at t=0.2s (index seg_len), +/- 15 ms.
zoom_span = int(0.015 * sr)
c = seg_len
lo, hi = c - zoom_span, c + zoom_span
tz = t[lo:hi] * 1000  # ms

axs[0, 0].plot(tz, hard[lo:hi])
axs[0, 0].set_title("Hard gain: waveform at transition")
axs[0, 0].set_xlabel("ms")
axs[0, 0].axvline(t[c] * 1000, color="r", ls="--", lw=0.8)

axs[0, 1].plot(tz, smooth[lo:hi])
axs[0, 1].set_title("Smoothed gain: waveform at transition")
axs[0, 1].set_xlabel("ms")
axs[0, 1].axvline(t[c] * 1000, color="r", ls="--", lw=0.8)

# Spectrograms: broadband click energy shows as vertical streaks at each 0.2s boundary.
axs[1, 0].specgram(hard, NFFT=1024, Fs=sr, noverlap=512)
axs[1, 0].set_title("Hard gain: spectrogram")
axs[1, 0].set_xlabel("s")
axs[1, 0].set_ylabel("Hz")

axs[1, 1].specgram(smooth, NFFT=1024, Fs=sr, noverlap=512)
axs[1, 1].set_title("Smoothed gain: spectrogram")
axs[1, 1].set_xlabel("s")
axs[1, 1].set_ylabel("Hz")

plt.tight_layout()
plt.savefig("proto/out/zipper_compare.png", dpi=130)

# Discontinuity metric: max sample-to-sample jump. Zipper noise shows up as
# large jumps concentrated at gain-change boundaries.
print("max |diff| hard:", np.max(np.abs(np.diff(hard))))
print("max |diff| smooth:", np.max(np.abs(np.diff(smooth))))
