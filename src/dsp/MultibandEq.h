#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "dsp/SvfBandNode.h"

/**
    16-band serial parametric EQ (Pro-Q-class channel strip) — the shape of the
    eventual graph-library node: prepare() off the audio thread, process() on it,
    reset() clears all band state.

    Owns 16 SvfBandNode in series (stereo-linked) and a per-band 10 ms LINEAR
    dry/wet crossfade so flipping a band enabled/disabled mid-signal doesn't click.

    Bypass rules (per spec, verified in proto/multiband_eq.py BypassableBand):
      - DISABLE: keep running the wet path while the blend gain ramps 1->0. The
        filter never goes silent; only its contribution to the output shrinks, so
        nothing abrupt for the ear. Once the gain reaches 0 the band is skipped
        entirely (~0 CPU).
      - ENABLE: reset() the band's state FIRST (so it doesn't resume from stale
        memory), then ramp the blend gain 0->1.

    Real-time contract: process() allocates nothing and takes no locks. The dry
    scratch used during a fade is pre-allocated in prepare(). Coefficient recompute
    (setBandParams) happens off the sample loop, once per block, only for bands
    the host layer knows are active.
*/
class MultibandEq {
public:
    static constexpr int kNumBands = 16;
    static constexpr int kMaxChannels = SvfBandNode::kMaxChannels;

    MultibandEq() = default;

    /** Off-thread. Sizes the dry-scratch buffer and prepares every band. */
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        fs = sampleRate;
        fadeLen = static_cast<int>(std::lround(sampleRate * kFadeMs / 1000.0));
        if (fadeLen < 1) fadeLen = 1;
        fadeStep = 1.0 / fadeLen;

        const int nCh = numChannels < kMaxChannels ? numChannels : kMaxChannels;
        maxBlock = maxBlockSize;
        // Pre-allocated OFF the audio thread. Flat [channel][sample] dry store,
        // only touched for a band that is mid-fade.
        dryScratch.assign(static_cast<std::size_t>(kMaxChannels) *
                              static_cast<std::size_t>(maxBlockSize),
                          0.0f);

        for (auto& b : bands)
            b.prepare(sampleRate, maxBlockSize, nCh);
    }

    /** Clear all band state and settle every fade to its target. */
    void reset() {
        for (auto& b : bands)
            b.reset();
        for (auto& f : fade)
            f.gain = f.target;
    }

    void setBandParams(int i, const SvfBandNode::BandParams& p) {
        bands[static_cast<std::size_t>(i)].setParams(p);
    }

    /** Toggle a band. immediate=true snaps the blend gain (used at prepare/load);
        otherwise it ramps over 10 ms. Enabling always resets the band's filter
        state first so it fades in from rest. */
    void setBandEnabled(int i, bool enabled, bool immediate = false) {
        Fade& f = fade[static_cast<std::size_t>(i)];
        if (enabled && f.target == 0.0)
            bands[static_cast<std::size_t>(i)].reset();
        f.target = enabled ? 1.0 : 0.0;
        if (immediate)
            f.gain = f.target;
    }

    /** A band is active (its wet path must run and its coefficients matter) when
        its blend gain or target is above zero. Fully-off bands are skipped. */
    bool isBandActive(int i) const {
        const Fade& f = fade[static_cast<std::size_t>(i)];
        return f.gain > 0.0 || f.target > 0.0;
    }

    void process(float* const* channels, int numChannels, int numSamples) {
        const int nCh = numChannels < kMaxChannels ? numChannels : kMaxChannels;

        for (std::size_t b = 0; b < kNumBands; ++b) {
            Fade& f = fade[b];
            const bool atRest = (f.gain == f.target);

            if (atRest && f.gain == 0.0)
                continue;  // fully disabled — ~0 CPU

            if (atRest && f.gain == 1.0) {
                bands[b].process(channels, numChannels, numSamples);  // fully wet
                continue;
            }

            // Mid-fade: stash dry, run wet in place, then blend with a per-sample
            // linear ramp shared across channels (stereo-linked).
            for (int ch = 0; ch < nCh; ++ch) {
                float* dst = dryPtr(ch);
                const float* src = channels[ch];
                for (int i = 0; i < numSamples; ++i)
                    dst[i] = src[i];
            }

            bands[b].process(channels, numChannels, numSamples);

            double a = f.gain;
            for (int i = 0; i < numSamples; ++i) {
                if (a < f.target)      a = a + fadeStep < f.target ? a + fadeStep : f.target;
                else if (a > f.target) a = a - fadeStep > f.target ? a - fadeStep : f.target;
                for (int ch = 0; ch < nCh; ++ch) {
                    const double dry = static_cast<double>(dryPtr(ch)[i]);
                    const double wet = static_cast<double>(channels[ch][i]);
                    channels[ch][i] = static_cast<float>(dry + a * (wet - dry));
                }
            }
            f.gain = a;
        }
    }

private:
    static constexpr double kFadeMs = 10.0;

    struct Fade {
        double gain = 0.0;    // current blend (0 = dry, 1 = wet)
        double target = 0.0;  // 0 disabled, 1 enabled
    };

    float* dryPtr(int ch) {
        return dryScratch.data() + static_cast<std::size_t>(ch) *
                                       static_cast<std::size_t>(maxBlock);
    }

    double fs = 44100.0;
    int fadeLen = 441;
    double fadeStep = 1.0 / 441.0;
    int maxBlock = 0;

    std::array<SvfBandNode, kNumBands> bands{};
    std::array<Fade, kNumBands> fade{};
    std::vector<float> dryScratch;
};
