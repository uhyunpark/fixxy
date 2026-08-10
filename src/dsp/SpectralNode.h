#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "dsp/SpectralFft.h"

/**
    Spectral dynamic-resonance suppressor (soothe-class) node — Stage 4, a
    faithful C++ port of the VERIFIED protos:
      - proto/spectral_stft.py       : streaming STFT/ISTFT framing, latency = N.
      - proto/spectral_core.py       : envelope -> exceed -> tanh soft-knee gain
                                       computer -> frequency-axis smoothing.
      - proto/spectral_ballistics.py : per-bin, frame-rate attack/release.
    This node re-derives NOTHING; every equation and constant comes from those
    files. The "why" comments below are lifted from their docstrings for the
    reader; the protos are ground truth.

    WHAT IT DOES, per hop (sqrt-Hann analysis + synthesis, 75% overlap):
      1. window the sliding frame, forward rfft -> per-bin complex spectrum;
      2. build a per-bin real GAIN from the magnitude:
           env   = octave-median background   (FastOctaveMedian)
           exceed= 20*log10((mag+EPS)/(env+EPS))   dB the bin stands over its
                   local background — a coherent tone stands ~28 dB over the
                   per-bin noise floor, random noise bins only a few dB, which is
                   what makes a *relative* processor cut resonances but leave
                   broadband material alone;
           red   = max_db * 0.5*(1 + tanh((exceed-knee_c)/knee_w))   soft-knee,
                   saturating — noise bins fall far below knee_c and are untouched;
           red   = octave boxcar smooth of red across bins (notch shape/width);
           red   = per-bin frame-rate attack/release on red (time smoothing);
           gain  = 10^(-red/20);
      3. apply gain to the spectrum, inverse rfft, synthesis-window, overlap-add.

    STEREO LINK v1 = LINKED COMMON GAIN MAP: the detection magnitude is the
    element-wise MAX across channels of |spec| (mono reduces to itself). ONE gain
    map is computed from that and applied to BOTH channels' spectra, so a
    resonance in either channel is cut identically in both — no image shift, and
    only one shared per-bin ballistics state. Each channel then does its own
    inverse transform + OLA.

    IDENTITY: depth == 0 => active_ == false => gain is identically 1 and the
    ballistics state is never touched, so the node is the pure STFT round trip:
    output == input delayed by exactly N samples (bit-near identity, limited only
    by the radix-2 FFT round-trip, ~1e-13).

    LATENCY = N (== kFftSize). This is the streaming framing's PROVEN latency
    (spectral_stft.py module docstring): not the offline N-hop figure but a full
    frame, the extra hop being what makes same-length-in/same-length-out correct
    under ARBITRARY host block sizes. Report it to the host.

    src/dsp CONTRACT: prepare() does all allocation and all exp()/tanh-constant /
    interp coefficient work happens in setParams() (NEVER per hop, NEVER per
    sample); process() is allocation-, lock-, IO-, logging-free with bounded work
    (<= numSamples/hop + 1 hops per call). Double state throughout; JUCE-free,
    atomic-free, header-only.
*/
class SpectralNode {
public:
    static constexpr int kFftSize = 1024;              // N (v1 fixed; designed so N can grow later)
    static constexpr int kHop = 256;                   // 75% overlap
    static constexpr int kNumBins = kFftSize / 2 + 1;  // 513
    static constexpr int kMaxChannels = 2;

    // ---- FROZEN calibrated constants (spectral_core.calibrate(), verified
    // reproducible; the dump script asserts calibrate() still reproduces these
    // within 1e-9 so scipy drift is caught, and the console test asserts these
    // literals equal the dumped values). These ARE the Stage-4 spec. ----
    static constexpr double kMaxDb = 16.167635300821875;     // reduction ceiling @ depth5/detail5
    static constexpr double kKneeC = 18.78301217932193;      // tanh knee centre (true-exceed dB)
    static constexpr double kKneeW = 9.010408026990198;      // tanh knee width (dB)
    static constexpr double kW0 = 0.07788596060802633;       // smoother width (oct) @ detail 0
    static constexpr double kW5 = 0.08630905922389523;       //                       @ detail 5
    static constexpr double kW10 = 0.005;                    //                       @ detail 10 (resolution floor)
    static constexpr double kM0 = 0.2719078891498013;        // depth multiplier @ detail 0
    static constexpr double kM5 = 1.0;                        //                  @ detail 5 (normalised)
    static constexpr double kM10 = 0.9067617761812925;       //                  @ detail 10
    static constexpr double kEnvHalfwidthOct = 0.5;          // octave-median window half-width
    static constexpr double kEps = 1e-12;
    static constexpr int kEnvGridM = 512;                    // log-freq resample grid size (proto M)

    // Detail knob anchors (log-linear interpolation over these).
    static constexpr double kDetailAnchor0 = 0.0;
    static constexpr double kDetailAnchor1 = 5.0;
    static constexpr double kDetailAnchor2 = 10.0;

    struct Params {
        double depth = 5.0;      // 0 => bit-near identity; 5 => calibrated curves
        double detail = 5.0;     // sharpness/selectivity: sets smoother width AND depth mult
        double attackMs = 27.0;  // measured soothe3 default (knob 5)
        double releaseMs = 50.0; // measured soothe3 default (knob 5)
    };

    SpectralNode() = default;

    /** Off-thread. Allocates every buffer (see the allocation list in the port
        report) and precomputes the fs-dependent framing + envelope maps. */
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        fs_ = sampleRate;
        maxBlock_ = maxBlockSize;
        activeChannels_ = numChannels < kMaxChannels ? numChannels : kMaxChannels;

        fft_.prepare(kFftSize);

        // sqrt-Hann analysis + synthesis (periodic Hann: /N, not /(N-1)).
        for (int n = 0; n < kFftSize; ++n) {
            const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n)
                                                     / static_cast<double>(kFftSize));
            const double w = std::sqrt(hann);
            analysisWin_[static_cast<std::size_t>(n)] = w;
            synthWin_[static_cast<std::size_t>(n)] = w;
        }
        norm_ = colaNorm();

        // Input FIFO capacity: at most a full block plus one hop of leftover.
        fifoCap_ = maxBlockSize + kHop;
        // Output ring: must exceed the live span of absolute output positions
        // touched within one process() call (~blockSize + hop), with a full-frame
        // margin. A small multiple of N over the block size is plenty.
        ringSize_ = maxBlockSize + 2 * kFftSize;

        for (int ch = 0; ch < kMaxChannels; ++ch) {
            inFifo_[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(fifoCap_), 0.0);
            outBuf_[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(ringSize_), 0.0);
        }

        prepareEnvelopeMaps();
        medScratch_.assign(static_cast<std::size_t>(envK_), 0.0);

        setParams(params_);
        reset();
    }

    /** Clear all framing + envelope + ballistics state. Safe before an enable
        fade-in. */
    void reset() {
        inHead_ = 0;
        inCount_ = 0;
        hopsDone_ = 0;
        totalEmitted_ = 0;
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            inputRing_[static_cast<std::size_t>(ch)].fill(0.0);
            olaBuffer_[static_cast<std::size_t>(ch)].fill(0.0);
            std::fill(outBuf_[static_cast<std::size_t>(ch)].begin(), outBuf_[static_cast<std::size_t>(ch)].end(), 0.0);
        }
        for (int b = 0; b < kNumBins; ++b) {
            ballS1_[static_cast<std::size_t>(b)] = 0.0;
            ballY1_[static_cast<std::size_t>(b)] = 0.0;
        }
    }

    /** Resolve every coefficient from params: the detail-driven smoother width +
        depth multiplier (log-linear interp), the scaled reduction ceiling, the
        octave smoother bin windows, and the frame-rate ballistics alphas. ALL the
        exp()/tanh-constant/interp work lives here — NEVER call per hop. */
    void setParams(const Params& p) {
        params_ = p;
        active_ = p.depth > 0.0;

        // Detail curves (log-linear interp over anchors 0/5/10), from spectral_core.
        const double widthOct = interpDetailLog(p.detail, kW0, kW5, kW10);
        const double depthMult = interpDetailLog(p.detail, kM0, kM5, kM10);
        maxDbScaled_ = kMaxDb * (p.depth / 5.0) * depthMult;

        // Octave smoother bin windows for the reduction curve (half-width = width/2).
        buildOctaveWindows(widthOct * 0.5, smootherLo_, smootherHi_);

        // Frame-rate ballistics: the control quantity only exists once per HOP, so
        // the smoother's "sample rate" is the FRAME rate fs/hop (spectral_ballistics).
        const double fsFrame = fs_ / static_cast<double>(kHop);
        aA_ = tauToAlpha(p.attackMs / 1000.0, fsFrame);
        aR_ = tauToAlpha(p.releaseMs / 1000.0, fsFrame);
    }

    const Params& getParams() const { return params_; }
    double getMaxDbScaled() const { return maxDbScaled_; }  // scaled reduction ceiling (for tests/UI)

    /** Audio thread. Pushes numSamples/channel, drains whole hops, emits exactly
        numSamples/channel of output delayed by N. Allocation-free. */
    void process(float* const* channels, int numChannels, int numSamples) {
        const int nCh = numChannels < activeChannels_ ? numChannels : activeChannels_;

        // 1) Push this block's samples into each channel's FIFO (shared timing).
        for (int i = 0; i < numSamples; ++i) {
            const std::size_t slot = static_cast<std::size_t>((inHead_ + inCount_) % fifoCap_);
            for (int ch = 0; ch < nCh; ++ch)
                inFifo_[static_cast<std::size_t>(ch)][slot] = static_cast<double>(channels[ch][i]);
            ++inCount_;
        }

        // 2) Drain every whole hop that has accumulated.
        while (inCount_ >= kHop)
            processOneHop(nCh);

        // 3) Emit numSamples/channel. Output position j reads absolute out-buf
        //    position (j - N); the first N samples are the causal pre-roll (0).
        for (int i = 0; i < numSamples; ++i) {
            const long long j = totalEmitted_;
            if (j >= kFftSize) {
                const std::size_t idx = static_cast<std::size_t>((j - kFftSize) % ringSize_);
                for (int ch = 0; ch < nCh; ++ch)
                    channels[ch][i] = static_cast<float>(outBuf_[static_cast<std::size_t>(ch)][idx]);
            } else {
                for (int ch = 0; ch < nCh; ++ch)
                    channels[ch][i] = 0.0f;
            }
            ++totalEmitted_;
        }
    }

    static constexpr int latencySamples() { return kFftSize; }

private:
    static constexpr double kPi = 3.14159265358979323846;

    // ------------------------------------------------------------------
    // One hop: frame -> forward FFT (per ch) -> shared gain -> inverse FFT +
    // OLA (per ch) -> position-indexed write. Verbatim framing from
    // spectral_stft.py StreamingSTFT._process_one_hop, extended to N channels
    // sharing one gain map (spectral_core / spectral_ballistics frame_gain).
    // ------------------------------------------------------------------
    void processOneHop(int nCh) {
        // (a) Per channel: slide the analysis ring by hop, window, forward FFT.
        for (int ch = 0; ch < nCh; ++ch) {
            auto& ring = inputRing_[static_cast<std::size_t>(ch)];
            // ring = concat(ring[hop:], new hop) — shift left by hop, refill tail.
            for (int n = 0; n < kFftSize - kHop; ++n)
                ring[static_cast<std::size_t>(n)] = ring[static_cast<std::size_t>(n + kHop)];
            for (int j = 0; j < kHop; ++j) {
                const std::size_t slot = static_cast<std::size_t>((inHead_ + j) % fifoCap_);
                ring[static_cast<std::size_t>(kFftSize - kHop + j)] = inFifo_[static_cast<std::size_t>(ch)][slot];
            }
            for (int n = 0; n < kFftSize; ++n)
                frame_[static_cast<std::size_t>(n)] =
                    ring[static_cast<std::size_t>(n)] * analysisWin_[static_cast<std::size_t>(n)];
            fft_.forward(frame_.data(), specRe_[static_cast<std::size_t>(ch)].data(), specIm_[static_cast<std::size_t>(ch)].data());
        }
        // Consume the hop from the shared FIFO.
        inHead_ = (inHead_ + kHop) % fifoCap_;
        inCount_ -= kHop;

        // (b) Shared gain map from the max-across-channels magnitude, applied to
        //     every channel's spectrum. Skipped entirely when inactive (identity).
        if (active_) {
            for (int b = 0; b < kNumBins; ++b) {
                double m = 0.0;
                for (int ch = 0; ch < nCh; ++ch) {
                    const double mag = std::hypot(specRe_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(b)],
                                                  specIm_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(b)]);
                    if (mag > m) m = mag;
                }
                detMag_[static_cast<std::size_t>(b)] = m;
            }
            computeGain();  // detMag_ -> gain_
            for (int ch = 0; ch < nCh; ++ch)
                for (int b = 0; b < kNumBins; ++b) {
                    const double g = gain_[static_cast<std::size_t>(b)];
                    specRe_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(b)] *= g;
                    specIm_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(b)] *= g;
                }
        }

        // (c) The h-th completed hop finalises absolute output positions
        //     [h*hop - N, h*hop - N + hop). Negative starts are the causal
        //     pre-roll and are simply not written (the ring's zero default is
        //     already correct there).
        ++hopsDone_;
        const long long start = hopsDone_ * kHop - kFftSize;

        for (int ch = 0; ch < nCh; ++ch) {
            fft_.inverse(specRe_[static_cast<std::size_t>(ch)].data(), specIm_[static_cast<std::size_t>(ch)].data(), frame_.data());
            auto& ola = olaBuffer_[static_cast<std::size_t>(ch)];
            for (int n = 0; n < kFftSize; ++n)
                ola[static_cast<std::size_t>(n)] +=
                    frame_[static_cast<std::size_t>(n)] * synthWin_[static_cast<std::size_t>(n)];

            if (start >= 0) {
                auto& out = outBuf_[static_cast<std::size_t>(ch)];
                for (int j = 0; j < kHop; ++j) {
                    const std::size_t idx = static_cast<std::size_t>((start + j) % ringSize_);
                    out[idx] = ola[static_cast<std::size_t>(j)] * norm_;
                }
            }
            // ola = concat(ola[hop:], zeros(hop)) — drain the finished hop.
            for (int n = 0; n < kFftSize - kHop; ++n)
                ola[static_cast<std::size_t>(n)] = ola[static_cast<std::size_t>(n + kHop)];
            for (int n = kFftSize - kHop; n < kFftSize; ++n)
                ola[static_cast<std::size_t>(n)] = 0.0;
        }
    }

    // ------------------------------------------------------------------
    // Gain map: env -> exceed -> tanh reduction -> octave smooth -> per-bin
    // ballistics -> 10^(-red/20). Reads detMag_, writes gain_. Shared per-bin
    // ballistics state (one s1/y1 set) because the gain map itself is shared.
    // ------------------------------------------------------------------
    void computeGain() {
        fastOctaveMedian();  // detMag_ -> env_

        for (int b = 0; b < kNumBins; ++b) {
            const double mag = detMag_[static_cast<std::size_t>(b)];
            const double env = env_[static_cast<std::size_t>(b)];
            const double exceed = 20.0 * std::log10((mag + kEps) / (env + kEps));
            const double red = maxDbScaled_ * 0.5
                             * (1.0 + std::tanh((exceed - kKneeC) / kKneeW));
            redRaw_[static_cast<std::size_t>(b)] = red;
        }

        // Octave boxcar smoothing of the reduction curve (prefix sum).
        csum_[0] = 0.0;
        for (int b = 0; b < kNumBins; ++b)
            csum_[static_cast<std::size_t>(b + 1)] =
                csum_[static_cast<std::size_t>(b)] + redRaw_[static_cast<std::size_t>(b)];
        for (int b = 0; b < kNumBins; ++b) {
            const int lo = smootherLo_[static_cast<std::size_t>(b)];
            const int hi = smootherHi_[static_cast<std::size_t>(b)];
            const double count = static_cast<double>(hi - lo + 1);
            const double sm = (csum_[static_cast<std::size_t>(hi + 1)]
                               - csum_[static_cast<std::size_t>(lo)]) / count;
            redSmooth_[static_cast<std::size_t>(b)] = sm;
        }

        // Per-bin frame-rate attack/release (smooth_decoupled_rtt), then gain.
        for (int b = 0; b < kNumBins; ++b) {
            const double x = redSmooth_[static_cast<std::size_t>(b)];
            double& s1 = ballS1_[static_cast<std::size_t>(b)];
            double& y1 = ballY1_[static_cast<std::size_t>(b)];
            const double relPole = aR_ * (s1 - x) + x;
            s1 = x > relPole ? x : relPole;   // s1 = max(x, aR*(s1-x)+x)
            const double y = aA_ * (y1 - s1) + s1;
            y1 = y;
            gain_[static_cast<std::size_t>(b)] = std::pow(10.0, -y / 20.0);
        }
    }

    // FastOctaveMedian (spectral_core): log-frequency resample -> fixed-size
    // median filter (mode="nearest", edge replication) -> resample back. The
    // interp index/weight maps and k are precomputed in prepare; DC passes
    // through. detMag_ -> env_.
    void fastOctaveMedian() {
        const int P = kNumBins - 1;   // positive bins (exclude DC): 512
        // Resample |mag| (positive bins) onto the uniform log-f grid.
        for (int g = 0; g < kEnvGridM; ++g) {
            const int idx = fwdIdx_[static_cast<std::size_t>(g)];
            const double w = fwdW_[static_cast<std::size_t>(g)];
            const double a = detMag_[static_cast<std::size_t>(idx + 1)];       // +1: skip DC
            const double bnext = detMag_[static_cast<std::size_t>(idx + 2)];
            onGrid_[static_cast<std::size_t>(g)] = a + w * (bnext - a);
        }
        // Centred median filter, size k, edges clamped (mode="nearest").
        const int khalf = envK_ / 2;
        for (int g = 0; g < kEnvGridM; ++g) {
            for (int w = -khalf; w <= khalf; ++w) {
                int idx = g + w;
                if (idx < 0) idx = 0;
                if (idx > kEnvGridM - 1) idx = kEnvGridM - 1;
                medScratch_[static_cast<std::size_t>(w + khalf)] =
                    onGrid_[static_cast<std::size_t>(idx)];
            }
            std::nth_element(medScratch_.begin(),
                             medScratch_.begin() + khalf,
                             medScratch_.end());
            med_[static_cast<std::size_t>(g)] = medScratch_[static_cast<std::size_t>(khalf)];
        }
        // Resample the median back onto the positive bins; DC passes through.
        env_[0] = detMag_[0];
        for (int i = 0; i < P; ++i) {
            const int idx = bwdIdx_[static_cast<std::size_t>(i)];
            const double w = bwdW_[static_cast<std::size_t>(i)];
            const double a = med_[static_cast<std::size_t>(idx)];
            const double bnext = med_[static_cast<std::size_t>(idx + 1)];
            env_[static_cast<std::size_t>(i + 1)] = a + w * (bnext - a);
        }
    }

    // ------------------------------------------------------------------
    // prepare-time helpers
    // ------------------------------------------------------------------

    // COLA normalisation: 1 / (the constant sqrt-Hann*sqrt-Hann OLA-sums to at
    // this (N, hop)). Replicates spectral_stft.cola_norm exactly (reps=8, middle
    // period). For N=1024/hop=256 this is 0.5.
    double colaNorm() const {
        constexpr int reps = 8;
        const int totalLen = kFftSize + reps * kHop;
        std::vector<double> acc(static_cast<std::size_t>(totalLen), 0.0);
        for (int k = 0; k < reps; ++k)
            for (int n = 0; n < kFftSize; ++n) {
                // window product == periodic Hann.
                const double prod = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n)
                                                         / static_cast<double>(kFftSize));
                acc[static_cast<std::size_t>(k * kHop + n)] += prod;
            }
        const int mid = (reps / 2) * kHop;
        double sum = 0.0;
        for (int j = 0; j < kHop; ++j)
            sum += acc[static_cast<std::size_t>(mid + j)];
        return static_cast<double>(kHop) / sum;  // 1 / mean
    }

    // Octave-proportional [lo,hi] bin windows, ±halfwidthOct around each bin
    // (spectral_core._octave_bin_windows). df = fs/N; DC maps to itself.
    void buildOctaveWindows(double halfwidthOct,
                            std::array<int, kNumBins>& lo,
                            std::array<int, kNumBins>& hi) const {
        const double up = std::pow(2.0, halfwidthOct);
        const double df = fs_ / static_cast<double>(kFftSize);  // == freqs[1]
        for (int k = 0; k < kNumBins; ++k) {
            const double f = static_cast<double>(k) * df;
            if (f <= 0.0) {
                lo[static_cast<std::size_t>(k)] = 0;
                hi[static_cast<std::size_t>(k)] = 0;
                continue;
            }
            const double flo = f / up;
            const double fhi = f * up;
            int l = static_cast<int>(std::floor(flo / df));
            int h = static_cast<int>(std::ceil(fhi / df));
            if (l < 0) l = 0;
            if (h > kNumBins - 1) h = kNumBins - 1;
            lo[static_cast<std::size_t>(k)] = l;
            hi[static_cast<std::size_t>(k)] = h;
        }
    }

    // Precompute the FastOctaveMedian log-freq grid, filter size k, and the two
    // np.interp index/weight maps (forward: grid<-logf/mag; backward: logf<-grid).
    void prepareEnvelopeMaps() {
        const int P = kNumBins - 1;  // positive bins
        const double df = fs_ / static_cast<double>(kFftSize);

        logf_.assign(static_cast<std::size_t>(P), 0.0);
        for (int i = 0; i < P; ++i)
            logf_[static_cast<std::size_t>(i)] = std::log2(static_cast<double>(i + 1) * df);

        const double logf0 = logf_[0];
        const double logfLast = logf_[static_cast<std::size_t>(P - 1)];

        grid_.assign(static_cast<std::size_t>(kEnvGridM), 0.0);
        const double step = (logfLast - logf0) / static_cast<double>(kEnvGridM - 1);
        for (int g = 0; g < kEnvGridM; ++g)
            grid_[static_cast<std::size_t>(g)] = logf0 + step * static_cast<double>(g);
        grid_[static_cast<std::size_t>(kEnvGridM - 1)] = logfLast;  // numpy linspace pins the end

        const double octaves = logfLast - logf0;
        const double ptsPerOct = static_cast<double>(kEnvGridM) / octaves;
        int k = static_cast<int>(std::lround(2.0 * kEnvHalfwidthOct * ptsPerOct));
        k |= 1;                 // make odd
        if (k < 3) k = 3;       // and >= 3
        envK_ = k;

        // Forward map: for each grid point, the interval in logf (np.interp,
        // end-clamped) — used to resample |mag| onto the grid.
        fwdIdx_.assign(static_cast<std::size_t>(kEnvGridM), 0);
        fwdW_.assign(static_cast<std::size_t>(kEnvGridM), 0.0);
        for (int g = 0; g < kEnvGridM; ++g) {
            interpMap(grid_[static_cast<std::size_t>(g)], logf_,
                      fwdIdx_[static_cast<std::size_t>(g)], fwdW_[static_cast<std::size_t>(g)]);
        }
        // Backward map: for each positive bin's logf, the interval in the grid —
        // used to resample the median back to linear bins.
        bwdIdx_.assign(static_cast<std::size_t>(P), 0);
        bwdW_.assign(static_cast<std::size_t>(P), 0.0);
        for (int i = 0; i < P; ++i) {
            interpMap(logf_[static_cast<std::size_t>(i)], grid_,
                      bwdIdx_[static_cast<std::size_t>(i)], bwdW_[static_cast<std::size_t>(i)]);
        }
    }

    // np.interp interval lookup for query q against ascending breakpoints xp:
    // returns lower index and fractional weight so f(q) = fp[idx] + w*(fp[idx+1]-fp[idx]).
    // Clamps like numpy (q<=xp[0] -> idx 0,w 0; q>=xp[-1] -> idx len-2,w 1).
    static void interpMap(double q, const std::vector<double>& xp, int& idx, double& w) {
        const int last = static_cast<int>(xp.size()) - 1;
        if (q <= xp[0]) { idx = 0; w = 0.0; return; }
        if (q >= xp[static_cast<std::size_t>(last)]) { idx = last - 1; w = 1.0; return; }
        int j = 0;
        // xp is short and monotone; linear scan is fine (prepare-time only).
        while (j < last && !(q >= xp[static_cast<std::size_t>(j)]
                             && q <= xp[static_cast<std::size_t>(j + 1)]))
            ++j;
        idx = j;
        const double a = xp[static_cast<std::size_t>(j)];
        const double bnext = xp[static_cast<std::size_t>(j + 1)];
        w = (q - a) / (bnext - a);
    }

    // Log-linear interpolation of (v0,v5,v10) at detail anchors 0/5/10, evaluated
    // at `detail`. exp(linear-interp(detail, anchors, log(vals))) — spectral_core
    // _interp_detail. np.interp clamps outside [0,10].
    static double interpDetailLog(double detail, double v0, double v5, double v10) {
        const double l0 = std::log(v0), l5 = std::log(v5), l10 = std::log(v10);
        double lv;
        if (detail <= kDetailAnchor0) lv = l0;
        else if (detail >= kDetailAnchor2) lv = l10;
        else if (detail <= kDetailAnchor1)
            lv = l0 + (l5 - l0) * (detail - kDetailAnchor0) / (kDetailAnchor1 - kDetailAnchor0);
        else
            lv = l5 + (l10 - l5) * (detail - kDetailAnchor1) / (kDetailAnchor2 - kDetailAnchor1);
        return std::exp(lv);
    }

    // alpha = exp(-1/(tau*fs)); tau <= 1us => 0 (instant). Port of tau_to_alpha.
    static double tauToAlpha(double tauS, double fsFrame) {
        if (tauS <= 1e-6) return 0.0;
        return std::exp(-1.0 / (tauS * fsFrame));
    }

    // ---- config / coefficients (resolved off-thread) ----
    double fs_ = 44100.0;
    int maxBlock_ = 0;
    int activeChannels_ = kMaxChannels;
    Params params_{};
    bool active_ = true;

    double norm_ = 1.0;
    double maxDbScaled_ = kMaxDb;
    double aA_ = 0.0, aR_ = 0.0;

    int fifoCap_ = 0;
    int ringSize_ = 0;
    int envK_ = 3;

    SpectralFft fft_;

    std::array<double, kFftSize> analysisWin_{};
    std::array<double, kFftSize> synthWin_{};

    std::array<int, kNumBins> smootherLo_{};
    std::array<int, kNumBins> smootherHi_{};

    // Envelope maps (sized in prepare).
    std::vector<double> logf_, grid_;
    std::vector<int> fwdIdx_, bwdIdx_;
    std::vector<double> fwdW_, bwdW_;
    std::vector<double> medScratch_;

    // ---- state (cleared by reset) ----
    int inHead_ = 0;
    long long inCount_ = 0;
    long long hopsDone_ = 0;
    long long totalEmitted_ = 0;

    std::array<std::array<double, kFftSize>, kMaxChannels> inputRing_{};
    std::array<std::array<double, kFftSize>, kMaxChannels> olaBuffer_{};
    std::array<std::vector<double>, kMaxChannels> inFifo_{};
    std::array<std::vector<double>, kMaxChannels> outBuf_{};

    std::array<double, kNumBins> ballS1_{};
    std::array<double, kNumBins> ballY1_{};

    // ---- per-hop scratch (no allocation on the audio thread) ----
    std::array<double, kFftSize> frame_{};
    std::array<std::array<double, kNumBins>, kMaxChannels> specRe_{};
    std::array<std::array<double, kNumBins>, kMaxChannels> specIm_{};
    std::array<double, kNumBins> detMag_{};
    std::array<double, kNumBins> env_{};
    std::array<double, kNumBins> redRaw_{};
    std::array<double, kNumBins> redSmooth_{};
    std::array<double, kNumBins> gain_{};
    std::array<double, kNumBins + 1> csum_{};
    std::array<double, kEnvGridM> onGrid_{};
    std::array<double, kEnvGridM> med_{};
};
