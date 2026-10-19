#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "dsp/SpectralFft.h"
#include "dsp/SpectralWeight.h"
#include "dsp/SpectralControl.h"

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
    sample). The one exception is the weight table: setParams only flags it
    dirty, and the bounded recompute runs at the TOP of process() (NEVER inside
    a hop, NEVER per sample), deferred so it never coincides with a hop burst at
    small buffers (see computeWeightTable / process). process() is allocation-,
    lock-, IO-, logging-free with bounded work (<= numSamples/hop + 1 hops, plus
    at most one weight recompute, per call). Double state throughout; JUCE-free,
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

        // Weight-curve bands + range window (see dsp/SpectralWeight.h): summed
        // into a per-bin dB table (computeWeightTable) and added to exceed
        // before the knee in computeGain().
        std::array<spectralweight::WeightBand, spectralweight::kMaxWeightBands> bands{};
        double rangeLoHz = 20.0;
        double rangeHiHz = 20000.0;

        // ---- SIDEBAR globals (proto/spectral_sidebar.py). Every default is an
        // EXACT no-op, proto-proven (gate1 default-identity): with these the node
        // is bit-identical to the pre-sidebar shared-gain-map behaviour. ----
        //
        // scale group:
        double maxCutDb = 24.0;    // clamp on per-bin reduction (dB); >= 24 => OFF (no clamp)
        double wetTrimDb = 0.0;    // flat wet-path trim; NOT applied here — plugin's SpectralMix does it
        // stereo group:
        bool   msMode = false;     // L/R <-> M/S (energy-preserving /sqrt2) around the STFT; mono => no-op
        double linkPct = 100.0;    // detection blend: det = (link/100)*maxMag + (1-link/100)*mag_ch
        double focus = 0.0;        // per-channel depth bias: depth_ch = depth*(1 - max(0, sign_ch*focus))
        // tilt group (x(f) = clip(log2(f/1000)/log2(16), -1, 1)):
        double tiltAttack = 0.0;   // per-bin attack tau *= 2^(tiltAttack*x(f))
        double tiltRelease = 0.0;  // per-bin release tau *= 2^(tiltRelease*x(f))
        double tiltDetail = 0.0;   // per-bin depth mult *= clip(2^(tiltDetail*x(f)), ...), folded into max_db

        bool operator==(const Params&) const = default;
    };

    SpectralNode() = default;

    /** Off-thread. Allocates every buffer (see the allocation list in the port
        report) and precomputes the fs-dependent framing + envelope maps. */
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        fs_ = sampleRate;
        capAlpha_ = std::exp(-static_cast<double>(kHop) / (fs_ * 0.020));
        maxBlock_ = std::max(1, maxBlockSize);
        activeChannels_ = numChannels < kMaxChannels ? numChannels : kMaxChannels;
        sqrt2_ = std::sqrt(2.0);  // M/S energy-preserving constant (proto np.sqrt(2.0))

        fft_.prepare(kFftSize);

        // sqrt-Hann analysis + synthesis (periodic Hann: /N, not /(N-1)).
        double analysisWindowSum = 0.0;
        for (int n = 0; n < kFftSize; ++n) {
            const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n)
                                                     / static_cast<double>(kFftSize));
            const double w = std::sqrt(hann);
            analysisWin_[static_cast<std::size_t>(n)] = w;
            synthWin_[static_cast<std::size_t>(n)] = w;
            analysisWindowSum += w;
        }
        hardAmplitudeScale_ = 2.0 / analysisWindowSum;
        norm_ = colaNorm();

        // Input FIFO capacity: at most a full block plus one hop of leftover.
        fifoCap_ = maxBlock_ + kHop;
        // Output ring: must exceed the live span of absolute output positions
        // touched within one process() call (~blockSize + hop), with a full-frame
        // margin. A small multiple of N over the block size is plenty.
        ringSize_ = maxBlock_ + 2 * kFftSize;

        for (int ch = 0; ch < kMaxChannels; ++ch) {
            inFifo_[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(fifoCap_), 0.0);
            keyFifo_[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(fifoCap_), 0.0);
            outBuf_[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(ringSize_), 0.0);
        }
        keyCountFifo_.assign(static_cast<std::size_t>(fifoCap_), 0);

        prepareEnvelopeMaps();
        medScratch_.assign(static_cast<std::size_t>(envK_), 0.0);

        // Bin-static prewarp tangents tan(pi*f/fs) at f = k*df (df = fs/N). These
        // are fs/N-static, so we hoist them out of the per-band weight recompute
        // (Part 1). DC bin (f=0): tan(0)=0 exactly, matching the previous
        // evalSection(recipe, fs, 0) path bit-for-bit.
        const double dfTan = fs_ / static_cast<double>(kFftSize);
        for (int k = 0; k < kNumBins; ++k)
            tanTable_[static_cast<std::size_t>(k)] =
                std::tan(kPi * (static_cast<double>(k) * dfTan) / fs_);

        setParams(params_);           // resolves detail/ballistics + caches the weight subset
        computeWeightTable(params_);  // full synchronous rebuild — off the audio thread
        weightDirty_ = false;
        reset();
    }

    /** Clear all framing + envelope + ballistics state. Safe before an enable
        fade-in. */
    void reset() {
        inHead_ = 0;
        inCount_ = 0;
        hopsDone_ = 0;
        effectiveCapDb_ = maxCutDb_ >= 24.0 ? kLegacyCapBound : maxCutDb_;
        finalClampBypass_ = maxCutDb_ >= 24.0;
        totalEmitted_ = 0;
        prevActive_ = false;  // Fix 3: no stale active->inactive edge after reset
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            inputRing_[static_cast<std::size_t>(ch)].fill(0.0);
            keyRing_[static_cast<std::size_t>(ch)].fill(0.0);
            olaBuffer_[static_cast<std::size_t>(ch)].fill(0.0);
            std::fill(outBuf_[static_cast<std::size_t>(ch)].begin(), outBuf_[static_cast<std::size_t>(ch)].end(), 0.0);
        }
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            ballS1_[static_cast<std::size_t>(ch)].fill(0.0);
            ballY1_[static_cast<std::size_t>(ch)].fill(0.0);
        }
        for (int b = 0; b < kNumBins; ++b)
            redDb_[static_cast<std::size_t>(b)] = 0.0;  // ribbon reads 0 while idle/bypassed
    }

    /** Resolve every coefficient from params: the detail-driven smoother width +
        depth multiplier (log-linear interp), the scaled reduction ceiling, the
        octave smoother bin windows, and the frame-rate ballistics alphas. ALL the
        exp()/tanh-constant/interp work lives here — NEVER call per hop. */
    void setParams(const Params& p) {
        params_ = p;
        active_ = p.depth > 0.0;

        // Weight table depends only on (bands, rangeLoHz, rangeHiHz) and fs/N.
        // When that subset changes, only FLAG it dirty + cache the subset here;
        // the actual recompute (~150 us) is deferred to process() (Part 2) so it
        // never lands on a hop callback at small buffers. An unrelated
        // depth/detail/attack tweak during a UI drag pays nothing.
        if (p.bands != weightBands_ || p.rangeLoHz != weightLo_
            || p.rangeHiHz != weightHi_) {
            weightBands_ = p.bands;
            weightLo_ = p.rangeLoHz;
            weightHi_ = p.rangeHiHz;
            weightDirty_ = true;
        }

        // Detail curves (log-linear interp over anchors 0/5/10), from spectral_core.
        const double widthOct = interpDetailLog(p.detail, kW0, kW5, kW10);
        const double depthMult = interpDetailLog(p.detail, kM0, kM5, kM10);
        maxDbScaled_ = kMaxDb * (p.depth / 5.0) * depthMult;

        // Octave smoother bin windows for the reduction curve (half-width = width/2).
        buildOctaveWindows(widthOct * 0.5, smootherLo_, smootherHi_);

        // ---- SIDEBAR coefficient resolution (proto/spectral_sidebar.py). All the
        // per-bin array fills below are bounded (<= 2 * kNumBins) and off the hop;
        // at defaults every array collapses to the pre-sidebar scalar, bit-identically. ----
        msMode_ = p.msMode;
        maxCutDb_ = p.maxCutDb;
        if (hopsDone_ == 0) {
            effectiveCapDb_ = maxCutDb_ >= 24.0 ? kLegacyCapBound : maxCutDb_;
            finalClampBypass_ = maxCutDb_ >= 24.0;
        }

        // focus depth bias: sign(+1) = channel A (L/M), sign(-1) = channel B (R/S).
        // depth_ch = depth * (1 - max(0, sign*focus)). At focus=0 both == depth.
        const double biasA = 1.0 - std::max(0.0, +1.0 * p.focus);
        const double biasB = 1.0 - std::max(0.0, -1.0 * p.focus);
        const double depthA = p.depth * biasA;
        const double depthB = p.depth * biasB;
        focusedDepth_ = {depthA, depthB};
        coreActive_[0] = depthA > 0.0;
        coreActive_[1] = depthB > 0.0;

        // DUAL-CORE ENGAGE: only when the two channels can actually differ —
        // link < 100 (detection differs) OR focus != 0 (depth differs). Otherwise
        // keep the single shared gain-map path (one envelope, the hop's dominant
        // cost); the proto proves that path is bit-identical to two linked cores.
        const double link = p.linkPct / 100.0;
        linkFrac_ = link;
        const bool wantDual = (p.linkPct < 100.0) || (p.focus != 0.0);
        // shared->dual: seed BOTH channel ballistics states from the shared state
        // (channel 0), so the newly-split channel 1 resumes from the same place.
        // Discrete param change; the per-bin attack/release resettle in ~50 ms —
        // acceptable (soothe-class edits are not sample-accurate automation).
        if (wantDual && !dualCore_) {
            ballS1_[1] = ballS1_[0];
            ballY1_[1] = ballY1_[0];
        }
        // dual->shared adopts channel 0's state automatically (shared path uses [0]).
        dualCore_ = wantDual;

        // Per-bin max_db (per channel: focus biases depth; tilt_detail biases the
        // depth multiplier by frequency). tilt_detail == 0 => flat == the scalar
        // maxDbScaled path, per bin, bit-identical.
        if (p.tiltDetail != 0.0) {
            for (int b = 0; b < kNumBins; ++b) {
                const double x = tiltX(binFreq(b));
                const double dm = clampD(depthMult * std::pow(2.0, p.tiltDetail * x), 0.25, 4.0);
                maxDbArr_[0][static_cast<std::size_t>(b)] = kMaxDb * (depthA / 5.0) * dm;
                maxDbArr_[1][static_cast<std::size_t>(b)] = kMaxDb * (depthB / 5.0) * dm;
            }
        } else {
            const double a = kMaxDb * (depthA / 5.0) * depthMult;
            const double bb = kMaxDb * (depthB / 5.0) * depthMult;
            maxDbArr_[0].fill(a);
            maxDbArr_[1].fill(bb);
        }

        // Frame-rate ballistics: the control quantity only exists once per HOP, so
        // the smoother's "sample rate" is the FRAME rate fs/hop (spectral_ballistics).
        // Per-bin arrays when a tilt is engaged; else flat == the pre-sidebar scalar.
        const double fsFrame = fs_ / static_cast<double>(kHop);
        aA_ = tauToAlpha(p.attackMs / 1000.0, fsFrame);
        aR_ = tauToAlpha(p.releaseMs / 1000.0, fsFrame);
        if (p.tiltAttack != 0.0) {
            for (int b = 0; b < kNumBins; ++b) {
                const double tau = (p.attackMs / 1000.0) * std::pow(2.0, p.tiltAttack * tiltX(binFreq(b)));
                aAArr_[static_cast<std::size_t>(b)] = tauToAlpha(tau, fsFrame);
            }
        } else {
            aAArr_.fill(aA_);
        }
        if (p.tiltRelease != 0.0) {
            for (int b = 0; b < kNumBins; ++b) {
                const double tau = (p.releaseMs / 1000.0) * std::pow(2.0, p.tiltRelease * tiltX(binFreq(b)));
                aRArr_[static_cast<std::size_t>(b)] = tauToAlpha(tau, fsFrame);
            }
        } else {
            aRArr_.fill(aR_);
        }
    }

    const Params& getParams() const { return params_; }
    // Latest target-detector request, observed at the next completed hop.
    // Keep the existing reduction history when switching; default remains Soft.
    void setDetectionMode(int mode) noexcept { detectionMode_ = mode == 1 ? 1 : 0; }
    void setExternalSidechainEnabled(bool enabled) noexcept { externalSidechain_ = enabled; }
    double getMaxDbScaled() const { return maxDbScaled_; }  // scaled reduction ceiling (for tests/UI)

    /** Copy the current per-bin reduction curve (dB, >= 0) into dst[0..n).
        This is the last-processed hop's smoothed+ballistics reduction — the same
        quantity that becomes gain = 10^(-red/20) — exposed for the editor's
        reduction ribbon. Plain copy, NO atomics (src/dsp stays atomic-free; the
        plugin layer owns the cross-thread publish, see src/plugins/AGENTS.md).
        When inactive (depth == 0) every bin is 0. n is clamped to kNumBins. */
    void copyReductionDb(float* dst, int n) const {
        const int m = n < kNumBins ? n : kNumBins;
        for (int b = 0; b < m; ++b)
            dst[static_cast<std::size_t>(b)] = static_cast<float>(redDb_[static_cast<std::size_t>(b)]);
    }

    /** Audio thread. Pushes numSamples/channel, drains whole hops, emits exactly
        numSamples/channel of output delayed by N. Allocation-free. */
    void process(float* const* channels, int numChannels, int numSamples,
                 const float* const* key = nullptr, int keyChannels = 0) {
        // Hosts may exceed their prepare-time block estimate. Keep both FIFO
        // occupancy and the unread output-ring span within allocated capacity.
        const int nCh = std::min(numChannels, activeChannels_);
        for (int offset = 0; offset < numSamples;) {
            const int count = std::min(maxBlock_, numSamples - offset);
            std::array<float*, kMaxChannels> chunk{};
            std::array<const float*, kMaxChannels> keyChunk{};
            for (int ch = 0; ch < nCh; ++ch)
                chunk[static_cast<std::size_t>(ch)] = channels[ch] + offset;
            const int nKey = key == nullptr ? 0 : std::clamp(keyChannels, 0, kMaxChannels);
            for (int ch = 0; ch < nKey; ++ch)
                keyChunk[static_cast<std::size_t>(ch)] = key[ch] + offset;
            processChunk(chunk.data(), nCh, count, keyChunk.data(), nKey);
            offset += count;
        }
    }

    static constexpr int latencySamples() { return kFftSize; }

private:
    void processChunk(float* const* channels, int numChannels, int numSamples,
                      const float* const* key, int keyChannels) {
        const int nCh = numChannels < activeChannels_ ? numChannels : activeChannels_;

        // 0) Deferred weight-table recompute (Part 2). Done here, at the TOP of
        //    process (the light path), so it NEVER coincides with a hop callback
        //    at small buffers. Recompute now UNLESS this call will cross a hop
        //    boundary AND the block is small (< 128): then leave it dirty for the
        //    next call, which at small buffers is hop-free. The table used until
        //    the recompute lands is the previous one — acceptable: weight edits
        //    are block-discrete and the per-bin ballistics smooth the audible
        //    result. At numSamples >= 128 the per-block budget is large enough to
        //    just do both, so we never defer there. Bounded, allocation-free.
        if (weightDirty_) {
            const bool willHop = (inCount_ + numSamples) >= kHop;
            if (!(willHop && numSamples < 128)) {
                computeWeightTable(params_);
                weightDirty_ = false;
            }
        }

        // 1) Push this block's samples into each channel's FIFO (shared timing).
        for (int i = 0; i < numSamples; ++i) {
            const std::size_t slot = static_cast<std::size_t>((inHead_ + inCount_) % fifoCap_);
            for (int ch = 0; ch < nCh; ++ch)
                inFifo_[static_cast<std::size_t>(ch)][slot] = static_cast<double>(channels[ch][i]);
            for (int ch = 0; ch < kMaxChannels; ++ch)
                keyFifo_[static_cast<std::size_t>(ch)][slot] = ch < keyChannels ? key[ch][i] : 0.0;
            keyCountFifo_[slot] = keyChannels;
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

    static constexpr double kPi = 3.14159265358979323846;

    // Verified spectral_cap_wola.py policy: one shared 20 ms cap per hop.
    // Soft can exceed 40 dB; stable Off must bypass the final clamp exactly.
    static constexpr double kLegacyCapBound = 129.341082406575;
    void advanceMaxCut() {
        const double target = maxCutDb_ >= 24.0 ? kLegacyCapBound : maxCutDb_;
        effectiveCapDb_ = capAlpha_ * effectiveCapDb_ + (1.0-capAlpha_) * target;
        if (std::abs(effectiveCapDb_-target) <= 1e-6) effectiveCapDb_ = target;
        finalClampBypass_ = maxCutDb_ >= 24.0 && effectiveCapDb_ == kLegacyCapBound;
    }

    // ------------------------------------------------------------------
    // One hop: frame -> forward FFT (per ch) -> shared gain -> inverse FFT +
    // OLA (per ch) -> position-indexed write. Verbatim framing from
    // spectral_stft.py StreamingSTFT._process_one_hop, extended to N channels
    // sharing one gain map (spectral_core / spectral_ballistics frame_gain).
    // ------------------------------------------------------------------
    void processOneHop(int nCh) {
        advanceMaxCut();  // before either lane, including inactive hops
        // M/S is only meaningful with a true stereo pair; mono is a guard no-op.
        const bool ms = msMode_ && nCh == 2;
        const int keyChannels = keyCountFifo_[static_cast<std::size_t>((inHead_ + kHop - 1) % fifoCap_)];
        // Keep physical L/R history warm independently of detection enable.
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            auto& ring = keyRing_[static_cast<std::size_t>(ch)];
            for (int n = 0; n < kFftSize-kHop; ++n)
                ring[static_cast<std::size_t>(n)] = ring[static_cast<std::size_t>(n+kHop)];
            for (int n = 0; n < kHop; ++n)
                ring[static_cast<std::size_t>(kFftSize-kHop+n)] =
                    keyFifo_[static_cast<std::size_t>(ch)][static_cast<std::size_t>((inHead_+n)%fifoCap_)];
            if (externalSidechain_ && ch < keyChannels) {
                for (int n = 0; n < kFftSize; ++n)
                    keyFrame_[static_cast<std::size_t>(n)] = ring[static_cast<std::size_t>(n)]*analysisWin_[static_cast<std::size_t>(n)];
                fft_.forward(keyFrame_.data(), keyRe_[static_cast<std::size_t>(ch)].data(),
                             keyIm_[static_cast<std::size_t>(ch)].data());
            }
        }

        // (a) Per channel: slide the analysis ring by hop, window into frameCh_.
        for (int ch = 0; ch < nCh; ++ch) {
            auto& ring = inputRing_[static_cast<std::size_t>(ch)];
            // ring = concat(ring[hop:], new hop) — shift left by hop, refill tail.
            for (int n = 0; n < kFftSize - kHop; ++n)
                ring[static_cast<std::size_t>(n)] = ring[static_cast<std::size_t>(n + kHop)];
            for (int j = 0; j < kHop; ++j) {
                const std::size_t slot = static_cast<std::size_t>((inHead_ + j) % fifoCap_);
                ring[static_cast<std::size_t>(kFftSize - kHop + j)] = inFifo_[static_cast<std::size_t>(ch)][slot];
            }
            auto& fr = frameCh_[static_cast<std::size_t>(ch)];
            for (int n = 0; n < kFftSize; ++n)
                fr[static_cast<std::size_t>(n)] =
                    ring[static_cast<std::size_t>(n)] * analysisWin_[static_cast<std::size_t>(n)];
        }
        // M/S encode (energy-preserving /sqrt2), on the windowed frames — the ring
        // history stays L/R for the next hop's slide. Linear, so encoding the
        // windowed frame == windowing the encoded ring (proto encode_ms).
        if (ms) {
            auto& fA = frameCh_[0];
            auto& fB = frameCh_[1];
            for (int n = 0; n < kFftSize; ++n) {
                const double L = fA[static_cast<std::size_t>(n)];
                const double R = fB[static_cast<std::size_t>(n)];
                fA[static_cast<std::size_t>(n)] = (L + R) / sqrt2_;
                fB[static_cast<std::size_t>(n)] = (L - R) / sqrt2_;
            }
        }
        for (int ch = 0; ch < nCh; ++ch)
            fft_.forward(frameCh_[static_cast<std::size_t>(ch)].data(),
                         specRe_[static_cast<std::size_t>(ch)].data(),
                         specIm_[static_cast<std::size_t>(ch)].data());
        // Consume the hop from the shared FIFO.
        inHead_ = (inHead_ + kHop) % fifoCap_;
        inCount_ -= kHop;

        // (b) Gain. Skipped entirely when inactive (identity).
        if (active_) {
            if (dualCore_ && nCh == 2) {
                // DUAL CORE: per-channel link-blended detection + independent
                // ballistics. det_ch = link*maxMag + (1-link)*mag_ch.
                for (int b = 0; b < kNumBins; ++b) {
                    const std::size_t bs = static_cast<std::size_t>(b);
                    const double magA = detectorMagnitude(0, bs, nCh, keyChannels, ms);
                    const double magB = detectorMagnitude(1, bs, nCh, keyChannels, ms);
                    const double mx = magA > magB ? magA : magB;
                    detA_[bs] = linkFrac_ * mx + (1.0 - linkFrac_) * magA;
                    detB_[bs] = linkFrac_ * mx + (1.0 - linkFrac_) * magB;
                }
                computeGainImpl(detA_.data(), maxDbArr_[0].data(), 0, coreActive_[0],
                                gainA_.data(), redA_.data());
                computeGainImpl(detB_.data(), maxDbArr_[1].data(), 1, coreActive_[1],
                                gainB_.data(), redB_.data());
                for (int b = 0; b < kNumBins; ++b) {
                    const std::size_t bs = static_cast<std::size_t>(b);
                    specRe_[0][bs] *= gainA_[bs];
                    specIm_[0][bs] *= gainA_[bs];
                    specRe_[1][bs] *= gainB_[bs];
                    specIm_[1][bs] *= gainB_[bs];
                    // Published ribbon: the deeper of the two channel cuts.
                    redDb_[bs] = redA_[bs] > redB_[bs] ? redA_[bs] : redB_[bs];
                }
            } else {
                // SHARED CORE (linked common gain map): max-across-channels
                // detection, ONE gain map on every channel — one envelope. This
                // is the pre-sidebar path, bit-identical at sidebar defaults.
                for (int b = 0; b < kNumBins; ++b) {
                    double m = 0.0;
                    for (int ch = 0; ch < nCh; ++ch) {
                        const double mag = detectorMagnitude(ch, static_cast<std::size_t>(b), nCh, keyChannels, ms);
                        if (mag > m) m = mag;
                    }
                    detMag_[static_cast<std::size_t>(b)] = m;
                }
                computeGainImpl(detMag_.data(), maxDbArr_[0].data(), 0, active_,
                                gain_.data(), redDb_.data());
                for (int ch = 0; ch < nCh; ++ch)
                    for (int b = 0; b < kNumBins; ++b) {
                        const double g = gain_[static_cast<std::size_t>(b)];
                        specRe_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(b)] *= g;
                        specIm_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(b)] *= g;
                    }
            }
            prevActive_ = true;
        } else if (prevActive_) {
            // active -> inactive (depth dropped to 0): computeGain no longer runs,
            // so redDb_ (published to the editor's reduction ribbon) and the
            // ballistics state would otherwise freeze at their last values. Clear
            // them ONCE on the transition to restore the documented invariant
            // ("redDb reads 0 while idle") and so a later re-enable can't resume
            // from stale envelope memory. Cheap: one pass, only on the edge.
            for (int b = 0; b < kNumBins; ++b)
                redDb_[static_cast<std::size_t>(b)] = 0.0;
            for (int ch = 0; ch < kMaxChannels; ++ch) {
                ballS1_[static_cast<std::size_t>(ch)].fill(0.0);
                ballY1_[static_cast<std::size_t>(ch)].fill(0.0);
            }
            prevActive_ = false;
        }

        // (c) The h-th completed hop finalises absolute output positions
        //     [h*hop - N, h*hop - N + hop). Negative starts are the causal
        //     pre-roll and are simply not written (the ring's zero default is
        //     already correct there).
        ++hopsDone_;
        const long long start = hopsDone_ * kHop - kFftSize;

        // Decode each completed frame before accumulation. OLA history must
        // always be L/R: decoding after OLA would reinterpret earlier frames
        // in the new basis when msMode changes during their overlap.
        for (int ch = 0; ch < nCh; ++ch)
            fft_.inverse(specRe_[static_cast<std::size_t>(ch)].data(), specIm_[static_cast<std::size_t>(ch)].data(),
                         frameCh_[static_cast<std::size_t>(ch)].data());
        if (ms) {
            for (int n = 0; n < kFftSize; ++n) {
                const auto idx = static_cast<std::size_t>(n);
                const double M = frameCh_[0][idx];
                const double S = frameCh_[1][idx];
                frameCh_[0][idx] = (M + S) / sqrt2_;
                frameCh_[1][idx] = (M - S) / sqrt2_;
            }
        }
        for (int ch = 0; ch < nCh; ++ch) {
            auto& ola = olaBuffer_[static_cast<std::size_t>(ch)];
            for (int n = 0; n < kFftSize; ++n)
                ola[static_cast<std::size_t>(n)] +=
                    frameCh_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] * synthWin_[static_cast<std::size_t>(n)];
        }

        // Output write and drain, always in L/R regardless of the current mode.
        if (start >= 0) {
            for (int j = 0; j < kHop; ++j) {
                const std::size_t idx = static_cast<std::size_t>((start + j) % ringSize_);
                for (int ch = 0; ch < nCh; ++ch)
                    outBuf_[static_cast<std::size_t>(ch)][idx] =
                        olaBuffer_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(j)] * norm_;
            }
        }
        for (int ch = 0; ch < nCh; ++ch) {
            auto& ola = olaBuffer_[static_cast<std::size_t>(ch)];
            // ola = concat(ola[hop:], zeros(hop)) — drain the finished hop.
            for (int n = 0; n < kFftSize - kHop; ++n)
                ola[static_cast<std::size_t>(n)] = ola[static_cast<std::size_t>(n + kHop)];
            for (int n = kFftSize - kHop; n < kFftSize; ++n)
                ola[static_cast<std::size_t>(n)] = 0.0;
        }
    }

    double detectorMagnitude(int channel, std::size_t bin, int mainChannels,
                             int keyChannels, bool ms) const {
        const auto ch = static_cast<std::size_t>(channel);
        if (!externalSidechain_) return std::hypot(specRe_[ch][bin], specIm_[ch][bin]);
        if (keyChannels == 0) return 0.0;
        if (keyChannels == 1) return std::hypot(keyRe_[0][bin], keyIm_[0][bin]);
        if (mainChannels == 1) {
            const double a = std::hypot(keyRe_[0][bin], keyIm_[0][bin]);
            const double b = std::hypot(keyRe_[1][bin], keyIm_[1][bin]);
            return std::sqrt((a*a+b*b)*0.5);
        }
        if (ms) {
            const double sign = channel == 0 ? 1.0 : -1.0;
            return std::hypot((keyRe_[0][bin]+sign*keyRe_[1][bin])/sqrt2_,
                              (keyIm_[0][bin]+sign*keyIm_[1][bin])/sqrt2_);
        }
        return std::hypot(keyRe_[ch][bin], keyIm_[ch][bin]);
    }

    // ------------------------------------------------------------------
    // Gain map: env -> exceed -> tanh reduction -> octave smooth -> per-bin
    // ballistics -> 10^(-red/20). Reads detMag_, writes gain_. Shared per-bin
    // ballistics state (one s1/y1 set) because the gain map itself is shared.
    // ------------------------------------------------------------------
    // One core's gain map: mag (detection spectrum) -> env -> exceed -> tanh knee
    // -> (max_cut clamp) -> octave smooth -> per-bin ballistics -> 10^(-red/20).
    // Parameterized so the SHARED path (one call) and the DUAL path (two calls,
    // ball indices 0/1, per-channel maxDbArr) share one body. maxDbArr is per-bin
    // (folds focus depth + tilt_detail); aAArr_/aRArr_ are per-bin (fold the tilt
    // attack/release); at sidebar defaults these arrays are flat == the scalar
    // pre-sidebar coefficients, so this is bit-identical there.
    // An INACTIVE core (focus drove its depth to 0) returns unity gain and touches
    // no ballistics state (proto frame_gain: `if not active: return ones`).
    void computeGainImpl(const double* mag, const double* maxDbArr, int ballIdx,
                         bool coreActive, double* outGain, double* outRed) {
        if (!coreActive) {
            for (int b = 0; b < kNumBins; ++b) {
                outGain[b] = 1.0;
                outRed[b] = 0.0;
            }
            return;
        }
        const bool zeroExternalDemand = externalSidechain_
            && std::none_of(mag, mag+kNumBins, [](double value) { return value > 0.0; });
        if (zeroExternalDemand) {
            // Zero NEW demand, retaining the existing release recurrence below.
            redRaw_.fill(0.0);
        } else if (detectionMode_ == 1) {
            // proto/spectral_hard_wola.py: absolute peak amplitude referred to
            // the ACTUAL sqrt-Hann analysis window, not the COLA output norm.
            // Retain legacy focus, MAX link, weights, width and ms ballistics.
            // Hard ignores legacy Detail depth multiplier/tiltDetail strength.
            const bool hasSignal = std::any_of(mag, mag+kNumBins, [](double value) { return value > 0.0; });
            for (int b = 0; b < kNumBins; ++b) {
                const auto bs = static_cast<std::size_t>(b);
                const double amplitude = mag[b]*hardAmplitudeScale_*((b == 0 || b == kNumBins-1) ? 0.5 : 1.0);
                double red = spectral_control::hardReduction(amplitude,
                    focusedDepth_[static_cast<std::size_t>(ballIdx)], weightDb_[bs], hasSignal);
                if (maxCutDb_ < 24.0 && red > maxCutDb_) red = maxCutDb_;
                redRaw_[bs] = red;
            }
        } else {
            fastOctaveMedian(mag);  // mag -> env_

            for (int b = 0; b < kNumBins; ++b) {
                const double env = env_[static_cast<std::size_t>(b)];
                double exceed = 20.0 * std::log10((mag[b] + kEps) / (env + kEps));
                exceed += weightDb_[static_cast<std::size_t>(b)];  // per-bin weight-curve bias, before the knee
                double red = maxDbArr[b] * 0.5
                           * (1.0 + std::tanh((exceed - kKneeC) / kKneeW));
                if (maxCutDb_ < 24.0 && red > maxCutDb_)
                    red = maxCutDb_;  // scale: max_cut clamp, after knee, before smoother
                redRaw_[static_cast<std::size_t>(b)] = red;
            }
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
        auto& s1a = ballS1_[static_cast<std::size_t>(ballIdx)];
        auto& y1a = ballY1_[static_cast<std::size_t>(ballIdx)];
        for (int b = 0; b < kNumBins; ++b) {
            const std::size_t bs = static_cast<std::size_t>(b);
            const double x = redSmooth_[bs];
            double& s1 = s1a[bs];
            double& y1 = y1a[bs];
            const double relPole = aRArr_[bs] * (s1 - x) + x;
            s1 = x > relPole ? x : relPole;   // s1 = max(x, aR*(s1-x)+x)
            const double y = aAArr_[bs] * (y1 - s1) + s1;
            // Retain the capped output history, but preserve release state s1.
            // This bounds spectral frame gain, not instantaneous OLA samples.
            y1 = finalClampBypass_ ? y : std::min(y, effectiveCapDb_);
            outRed[b] = y1;
            outGain[b] = std::pow(10.0, -y1 / 20.0);
        }
    }

    // ------------------------------------------------------------------
    // Per-bin weight table (proto/spectral_weight.py weight_db): for each bin
    // centre f = k*fs/N,
    //     weightDb_[k] = gateDb(f, lo, hi)
    //                  + sum over enabled bands of the band's magnitude (dB).
    // Both terms come from the single-source evaluators in SpectralWeight.h
    // (shared with the editor's drawn curve). A BandWeightEval is built ONCE
    // per band (recipe hoisted out of the 513-bin loop), and tan(pi*f/fs) is
    // read from the precomputed tanTable_ (also hoisted). Runs at the top of
    // process() when the weight subset is dirty (setParams only flags it) —
    // deferred off hop callbacks; NEVER per hop or per sample. Allocation-free,
    // bounded (<= 8 bands x 513 bins of closed-form complex algebra).
    void computeWeightTable(const Params& p) {
        const double df = fs_ / static_cast<double>(kFftSize);

        for (int k = 0; k < kNumBins; ++k)
            weightDb_[static_cast<std::size_t>(k)] =
                spectralweight::gateDb(static_cast<double>(k) * df, p.rangeLoHz, p.rangeHiHz);

        for (const auto& band : p.bands) {
            if (!band.on) continue;
            const spectralweight::BandWeightEval eval(band, fs_);  // recipe hoisted once per band
            for (int k = 0; k < kNumBins; ++k)
                weightDb_[static_cast<std::size_t>(k)] +=
                    eval.db(static_cast<double>(k) * df, tanTable_[static_cast<std::size_t>(k)]);
        }
    }

    // FastOctaveMedian (spectral_core): log-frequency resample -> fixed-size
    // median filter (mode="nearest", edge replication) -> resample back. The
    // interp index/weight maps and k are precomputed in prepare; DC passes
    // through. detMag_ -> env_.
    void fastOctaveMedian(const double* mag) {
        const int P = kNumBins - 1;   // positive bins (exclude DC): 512
        // Resample |mag| (positive bins) onto the uniform log-f grid.
        for (int g = 0; g < kEnvGridM; ++g) {
            const int idx = fwdIdx_[static_cast<std::size_t>(g)];
            const double w = fwdW_[static_cast<std::size_t>(g)];
            const double a = mag[idx + 1];       // +1: skip DC
            const double bnext = mag[idx + 2];
            onGrid_[static_cast<std::size_t>(g)] = a + w * (bnext - a);
        }
        // Centred median filter, size k, edges clamped (mode="nearest"). An
        // incrementally-maintained SORTED window replaces the old per-grid-point
        // copy + std::nth_element: for the interior (window fully inside the grid)
        // the window slides by exactly one, so one value leaves and one enters,
        // each an O(k) lower_bound + shift into medScratch_ (the sorted buffer,
        // sized to envK_ in prepare). The edge-clamped points (first/last khalf,
        // where boundary values repeat and the slide isn't a clean single in/out)
        // fall back to a full rebuild+sort. This is bit-identical to the old path
        // (same median = element khalf of the sorted length-k window, same
        // clamped windows) — verified max|incremental - nth_element| = 0 over the
        // synthetic magnitude grid (scratchpad/median_algo_bench.cpp), and it is
        // the dominant hop cost (~82% of the envelope stage). med_[g] reads the
        // window's middle element (k is odd, so khalf = (k-1)/2 is the median).
        const int k = envK_;
        const int khalf = k / 2;
        double* const win = medScratch_.data();  // sorted window, length k

        auto rebuildAt = [&](int g) {
            for (int w = -khalf; w <= khalf; ++w) {
                int idx = g + w;
                if (idx < 0) idx = 0;
                if (idx > kEnvGridM - 1) idx = kEnvGridM - 1;
                win[w + khalf] = onGrid_[static_cast<std::size_t>(idx)];
            }
            std::sort(win, win + k);
        };

        const int loEdge = khalf;                 // g <= loEdge: left-clamped
        const int hiEdge = kEnvGridM - 1 - khalf;  // g >  hiEdge: right-clamped
        rebuildAt(0);
        med_[0] = win[khalf];
        for (int g = 1; g < kEnvGridM; ++g) {
            if (g <= loEdge || g > hiEdge) {
                rebuildAt(g);
            } else {
                // Interior: window slides by one. Erase the leaving value (present,
                // so lower_bound lands on it), then insert the entering value at
                // its sorted position — both O(k), buffer stays length k and sorted.
                const double leaving = onGrid_[static_cast<std::size_t>(g - 1 - khalf)];
                const double entering = onGrid_[static_cast<std::size_t>(g + khalf)];
                double* const itL = std::lower_bound(win, win + k, leaving);
                std::move(itL + 1, win + k, itL);          // shift tail down over the gap
                double* const itE = std::lower_bound(win, win + (k - 1), entering);
                std::move_backward(itE, win + (k - 1), win + k);  // open a slot
                *itE = entering;
            }
            med_[static_cast<std::size_t>(g)] = win[khalf];
        }
        // Resample the median back onto the positive bins; DC passes through.
        env_[0] = mag[0];
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

    // Bin centre frequency f = k*fs/N.
    double binFreq(int k) const {
        return static_cast<double>(k) * (fs_ / static_cast<double>(kFftSize));
    }

    // Tilt frequency axis: x(f) = clip(log2(f/1000)/log2(16), -1, 1); f<=0 (DC)
    // guarded to a tiny positive placeholder so it lands at the -1 floor, not -inf
    // (proto tilt_x). ±4 octaves around 1 kHz map to ±1.
    static double tiltX(double f) {
        const double safe = f > 1e-9 ? f : 1e-9;
        const double x = std::log2(safe / 1000.0) / std::log2(16.0);
        return x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x);
    }

    static double clampD(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // ---- config / coefficients (resolved off-thread) ----
    double fs_ = 44100.0;
    int maxBlock_ = 0;
    int activeChannels_ = kMaxChannels;
    Params params_{};
    int detectionMode_ = 0;
    bool externalSidechain_ = false;
    double hardAmplitudeScale_ = 1.0;
    std::array<double, kMaxChannels> focusedDepth_{{5.0, 5.0}};
    bool active_ = true;
    bool prevActive_ = false;  // Fix 3: tracks the active->inactive edge for redDb_ reset

    double norm_ = 1.0;
    double maxDbScaled_ = kMaxDb;
    double aA_ = 0.0, aR_ = 0.0;

    // ---- SIDEBAR resolved coefficients / state (set in setParams) ----
    double sqrt2_ = 1.4142135623730951;   // set in prepare = std::sqrt(2.0), for M/S
    bool msMode_ = false;
    bool dualCore_ = false;                // two independent cores engaged (link<100 || focus!=0)
    double maxCutDb_ = 24.0;               // >= 24 => clamp OFF
    double capAlpha_ = 0.0;
    double effectiveCapDb_ = kLegacyCapBound;
    bool finalClampBypass_ = true;
    double linkFrac_ = 1.0;                // linkPct/100
    std::array<bool, kMaxChannels> coreActive_{{true, true}};  // depth_ch > 0 per channel
    std::array<double, kNumBins> aAArr_{};  // per-bin attack alpha (flat == aA_ when tilt off)
    std::array<double, kNumBins> aRArr_{};  // per-bin release alpha (flat == aR_ when tilt off)
    std::array<std::array<double, kNumBins>, kMaxChannels> maxDbArr_{};  // per-channel per-bin max_db

    // Bin-static prewarp tangents tan(pi*f/fs), f = k*fs/N — filled in prepare,
    // hoisted out of the per-band weight recompute (Part 1).
    std::array<double, kNumBins> tanTable_{};

    // Weight table + the last weight-subset it was built from (change detection).
    std::array<double, kNumBins> weightDb_{};
    std::array<spectralweight::WeightBand, spectralweight::kMaxWeightBands> weightBands_{};
    double weightLo_ = 20.0;
    double weightHi_ = 20000.0;
    bool weightDirty_ = true;

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
    std::array<std::array<double, kFftSize>, kMaxChannels> keyRing_{};
    std::array<std::vector<double>, kMaxChannels> keyFifo_{};
    std::vector<int> keyCountFifo_;
    std::array<std::array<double, kFftSize>, kMaxChannels> olaBuffer_{};
    std::array<std::vector<double>, kMaxChannels> inFifo_{};
    std::array<std::vector<double>, kMaxChannels> outBuf_{};

    // Per-channel ballistics state (index 0 = shared/channel A, 1 = channel B).
    std::array<std::array<double, kNumBins>, kMaxChannels> ballS1_{};
    std::array<std::array<double, kNumBins>, kMaxChannels> ballY1_{};

    // ---- per-hop scratch (no allocation on the audio thread) ----
    std::array<std::array<double, kFftSize>, kMaxChannels> frameCh_{};  // windowed frames (M/S encode acts here)
    std::array<std::array<double, kNumBins>, kMaxChannels> specRe_{};
    std::array<std::array<double, kNumBins>, kMaxChannels> specIm_{};
    std::array<double, kFftSize> keyFrame_{};
    std::array<std::array<double, kNumBins>, kMaxChannels> keyRe_{}, keyIm_{};
    std::array<double, kNumBins> detMag_{};      // shared-path detection (max across channels)
    std::array<double, kNumBins> detA_{}, detB_{};  // dual-path per-channel detection
    std::array<double, kNumBins> gainA_{}, gainB_{};  // dual-path per-channel gain
    std::array<double, kNumBins> redA_{}, redB_{};    // dual-path per-channel reduction dB
    std::array<double, kNumBins> env_{};
    std::array<double, kNumBins> redRaw_{};
    std::array<double, kNumBins> redSmooth_{};
    std::array<double, kNumBins> gain_{};
    std::array<double, kNumBins> redDb_{};  // last-hop per-bin reduction dB (for the UI ribbon)
    std::array<double, kNumBins + 1> csum_{};
    std::array<double, kEnvGridM> onGrid_{};
    std::array<double, kEnvGridM> med_{};
};
