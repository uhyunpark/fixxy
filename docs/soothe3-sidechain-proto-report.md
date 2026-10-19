# External sidechain and independent listen — Stage 2

Date: 2026-10-17. Scope: add an external detector and independent raw-key
monitor to the production N=1024/H=256 periodic sqrt-Hann WOLA. These routing,
downmix and 5 ms monitor-transition choices are our design. Reference soothe3
remains unavailable; this is neither a proprietary algorithm claim nor sonic
parity. No existing prototype, fixture or tolerance was changed.

## Exact Stage 3 interface contract

Preserve the three-argument node call through default trailing arguments:

```cpp
void setExternalSidechainEnabled(bool enabled) noexcept;
void process(float* const* main, int mainChannels, int samples,
             const float* const* key = nullptr, int keyChannels = 0);
```

`mainChannels` is 1 or 2. `keyChannels` is 0, 1 or 2; count zero means missing.
For a positive count, each key pointer supplies exactly `samples` valid samples.
Missing input writes zeros into key history. Key audio must be captured before
any main/output bus overwrite if host buffers share storage. An ordinary main
bus layout must not accidentally expose the key channels as output channels.
Add boolean parameter IDs `externalSidechain` and `sidechainListen`, both false
by default. Detection enable and listen enable are independent.

Allocate a separate two-channel key FIFO, sliding key frame and key spectrum in
`prepare()`. Keep physical key channels L/R in these buffers. Match every main
sample/hop, including arbitrary host chunks. Continue ingesting connected key
while external detection is off and while any monitoring route is selected.
Missing key must never fall back to main detection while enabled. Source and
Soft/Hard changes retain the existing `s1/y1` state. Preserve legacy MAX linking;
power-domain linking belongs to a separate package.

Sample source enable and mode at each completed internal H boundary. A host
connection-count change is effective for samples in that call; each completed
hop uses the count attached to its final sample. This detail matters when a
host call changes connection count while an incomplete hop is in the FIFO.
FIFO sample/count handling must preserve that boundary. The source-enable
setter supplies the latest requested state when the next hop completes.

For plugin monitoring, add SC-listen as a fourth route alongside Normal, Delta
and Bypass. Use the existing latency-aligned dry buffer and a **separate raw
physical-key delay of N samples**, including when detection is disabled.
Prepare all delay storage off-thread. Keep the reported latency exactly N=1024.

## Equations to port verbatim

For each completed hop, append H main samples and H physical key samples into
their own N-sample histories. Analyze both with

```text
w[n] = sqrt(0.5 - 0.5*cos(2*pi*n/N)), n=0..N-1
X = rfft(main_frame * w)
K = rfft(key_physical_frame * w)
```

Encode stereo main with the existing orthonormal M/S transform iff M/S is on.
Select detector magnitudes before the existing legacy link blend:

```text
externalSidechain == false:
    detector_lanes = abs(X)                       // exact legacy path
externalSidechain == true, keyChannels == 0:
    detector_lanes = 0
externalSidechain == true, keyChannels == 1:
    detector_lane_A = abs(K_L)
    detector_lane_B = abs(K_L)                    // when main stereo, even M/S
externalSidechain == true, keyChannels == 2, mainChannels == 1:
    detector_lane_A = sqrt((abs(K_L)^2 + abs(K_R)^2)/2)
externalSidechain == true, keyChannels == 2, mainChannels == 2, M/S off:
    detector_lane_A = abs(K_L)
    detector_lane_B = abs(K_R)
externalSidechain == true, keyChannels == 2, mainChannels == 2, M/S on:
    detector_lane_A = abs((K_L + K_R)/sqrt(2))
    detector_lane_B = abs((K_L - K_R)/sqrt(2))
```

Mono key is broadcast directly into the detector domain. Duplicating it into
physical L/R and encoding M/S would erase its Side demand, which is incorrect
for this contract. Stereo-to-mono **detector** downmix is phase-insensitive
equal-power averaging, our explicit choice. Linear FFT scaling commutes with
this magnitude-domain operation; Hard calibration therefore remains exact.

Legacy stereo linking and focused local depth remain:

```text
dual = mainChannels == 2 && (linkPct < 100 || focus != 0)
dual detector_ch = (linkPct/100)*max(detector_A, detector_B)
                   + (1-linkPct/100)*detector_ch
nondual detector = max(detector_lanes)             // one shared gain/state
depth_A = depth*(1-max(0, focus))
depth_B = depth*(1-max(0, -focus))
```

When external detection is selected and an entire post-link detector lane is
exactly zero, set its new reduction target to zero **before** the Soft tanh.
Do not feed a floor into the Soft knee: it has a nonzero baseline at zero
exceed. Release existing state with the same frozen per-bin poles:

```text
target[k] = 0
s1[k] = aR[k]*s1[k]
y1[k] = aA[k]*(y1[k]-s1[k]) + s1[k]
gain[k] = 10^(-y1[k]/20)
```

Depth-zero inactive behavior remains legacy identity without state updates.
For a connected silent input, the earlier nonzero samples continue to belong
to the N-sample analysis window until they age out. Disconnection suppresses
new demand immediately at the next completed hop even if old nonzero samples
remain in its key frame. Existing release state is never cleared.

Otherwise Soft calls the frozen Sidebar target recipe unmodified:

```text
env = legacy log-frequency median envelope(detector)
exceed[k] = 20*log10((detector[k]+1e-12)/(env[k]+1e-12)) + weight[k]
raw[k] = maxDb[k]*0.5*(1+tanh((exceed[k]-kneeC)/kneeW))
```

Hard calls the already-verified absolute knee, retaining focused depth,
frequency width, time poles, weight/range and max-cut ordering:

```text
a[k] = 2*detector[k]/sum(w)                        // interior bins
a[0] *= 0.5; a[N/2] *= 0.5
z[k] = 20*log10(max(a[k],1e-12)) + weight[k] + 6 + 6*depth_ch
knee(z) = 0, z<=-3; (z+3)^2/12, -3<z<3; z, z>=3
raw[k] = clip(0.75*knee(z[k]), 0, 40)
```

Legacy `tiltDetail` remains Soft-only. For either nonzero target recipe:

```text
if maxCutDb < 24: raw[k] = min(raw[k], maxCutDb)
target[k] = mean(raw[lo[k] : hi[k]+1])            // existing width tables
s1[k] = max(target[k], aR[k]*(s1[k]-target[k])+target[k])
y1[k] = aA[k]*(y1[k]-s1[k])+s1[k]
gain[k] = 10^(-y1[k]/20)
```

Apply gain to **main X only**, then existing inverse FFT, optional M/S decode,
sqrt-Hann synthesis and 0.5-normalized OLA. Key audio is never a carrier.

Raw listen mapping is intentionally different from detector downmix:

```text
missing key: listen_raw = 0
mono key, stereo main: listen_raw_L = listen_raw_R = key_L
stereo key, mono main: listen_raw = (key_L+key_R)/2
stereo key, stereo main: listen_raw_LR = key_physical_LR
listen_aligned[n] = listen_raw[n-N]               // zero initial history
```

Do not M/S-transform the listen route. For already aligned signals, the four
candidate output streams are:

```text
Normal = ((1-mix)*dry + mix*wet*wetGain)*outGain
Delta = (wet*wetGain - dry)*outGain                // frozen polarity
ScListen = listen_aligned*outGain                 // ignores mix and wetGain
Bypass = dry                                     // ignores both gain controls
selected = Bypass if bypass else ScListen if sidechainListen else Delta if delta else Normal
```

Make route selection click-resistant using our 5 ms linear **route-weight**
crossfade, including entering/leaving bypass:

```text
L = max(1, floor(0.005*sampleRate + 0.5))
initial weights = one_hot(initial selected route)
on changed route request:
    target = one_hot(selected)
    step = (target-current_weights)/L
    remaining = L
for each output sample:
    if remaining > 0:
        current_weights += step
        remaining -= 1
        if remaining == 0: current_weights = target
    output = sum(current_weights[route]*candidate[route])
```

The request sample advances the first 1/L step; the Lth sample reaches the new
route exactly. On interrupted fades restart from the current weight vector,
not the last route's endpoint. Only a **changed selected route** restarts the
ramp; repeatedly setting the same booleans must not extend it indefinitely.
Parameter gain smoothing remains the existing plugin behavior. Monitoring
never resets/pauses detector or audio history. Steady-state bypass, listen and
Delta identities apply after the selection ramp has completed.

## Reproduction and measured gates

```sh
./proto/.venv/bin/python proto/spectral_sidechain.py
./proto/.venv/bin/python proto/dump_spectral_sidechain_fixtures.py
```

Both commands print numbered assertions. The dumper reruns every gate in the
same process before exporting. Maximum errors from the verified run:

| Gate | Maximum absolute error |
| --- | ---: |
| SC-off versus frozen Soft/Hard audio | 0 |
| Arbitrary chunks, including mode/source/connection switches | 0 |
| Direct full-bin DFT versus detector FFT magnitudes | 1.138402e-12 |
| Independent scalar Soft/Hard gain/state loop, source/mode switches | 8.474332e-13 |
| SciPy STFT/ISTFT carrier audio using independent gain decisions | 1.382575e-15 |
| Fixed key, carrier scaling by -0.37 | 5.551115e-17 |
| Mono key both M/S detector lanes | 0 |
| In-phase/antiphase stereo key into mono main | 0 |
| Zero-demand release recurrence | 0 |
| Key framing warmed while disabled | 0 |
| Depth-zero delayed main identity | 1.387779e-16 |
| Steady monitor precedence/null identities | 1.110223e-16 |
| Independently calculated interrupted crossfade weights | 2.664535e-15 |
| Independently calculated crossfade audio | 5.551115e-16 |

Gain and SciPy tolerances are 1e-10; most identity checks use 1e-12. The
independent scalar loop uses SciPy median and direct neighborhood means rather
than calling either production target/smoothing/state routine. Static frozen
coefficient tables and the established envelope recipe are shared. The direct
DFT explicitly sums all bins, avoiding FFT dependence; SciPy independently
reconstructs carrier audio with its own framing/normalization. A small known
noise floor keeps internal Soft control ratios numerically conditioned during
the source-switch oracle; exact pure M-only/S-only and silence routing have
separate assertions. No tolerance was loosened to make a failing oracle pass.

Other assertions cover fixed main/varying key, disconnected/silent key from
startup giving delayed-main identity, a key lane with zero energy in stereo
M/S, and listen invariance under mix/wet-trim/wet-signal changes. Soft responds
to spectral-shape changes; Hard additionally responds to key level changes.

The proto renders half-second 48 kHz float WAVs to
`proto/out/spectral_sidechain_{main,key,soft,hard}.wav`. These are synthetic
audition material, not a listening verdict.

## Exported fixtures

`test/spectral_sidechain_fixtures.h` is generated from float32-rounded main and
key inputs; expected values are float64. Input length is 1536 samples/channel;
audio output is 2560 including N leading latency zeros. Arrays are channel
major, and each output's channel count is explicit in its generated comments.

- `kSoft`, `kHard`: stereo main, stereo key, default parameters, SC enabled.
- `kMonoKeyMs`: stereo main M/S, mono key channel0, Hard, linkPct=0.
- `kStereoKeyMono`: mono main channel0, stereo key, Hard, default parameters.
- `kMissing`: stereo main, missing key, Soft, SC enabled.
- `kSwitch`: default parameters, initially internal Soft; completed input
  boundary768 selects external Hard; boundary1280 selects internal Soft.
  Connection changes occur **before input samples** 512->0ch, 768->1ch,
  1024->2ch, with 2ch initially. Keep count2 during the zero-padded tail.
- `kSoftTargetFrame4`/`kHardTargetFrame4` and matching gains pin all 513 bins
  of the pre-ballistics target and final gain at zero-based frame4, completing
  at input sample1280. These are independently scalar-computed and asserted
  against the streaming gain trajectory before export.
- `kAlignedKey` and `kMonitor` use the switched connection schedule. Monitor
  events before output samples1050=SC listen+Delta, 1150=Bypass+SC listen+Delta,
  1500=Delta, 1900=Normal. Mix=.37, outGain=.8, wetGain=1.4, L=240. The first
  fade is interrupted after 100 samples. This pins precedence and retargeting.

## Evidence limits and Stage 3 requirements

This stage verifies Python DSP/routing only. Production C++ remains untouched.
Python queues and growing arrays make no real-time allocation claim. Stage 3
must audit all additional callback work: no allocation/deallocation, locks,
I/O or logging; fixed channel/bin bounds; at most one additional physical-key
FFT per key channel per completed hop; preallocated delay/FIFO/ring storage.
Avoid scheduling these key FFTs while disabled if desired, but ingestion and
history must continue. Do not reset state to implement bypass or listen.

The C++ handoff must run old tests unchanged, new fixture tests across arbitrary
host chunks, layout negotiation/state persistence tests, a warning-clean build,
pluginval strictness5, and visual signal verification of new controls. This
report does not claim those gates, CPU limits, broad material tuning or original
product sound. The scalar oracle, routing and fixture schedules above are the
implementation contract; no FIR rewrite is authorized by this package.
