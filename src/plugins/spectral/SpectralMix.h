#pragma once
#include <algorithm>
#include <array>
#include <cmath>

/**
    LpngSpectral output-stage algebra, factored out of processBlock() as a pure,
    JUCE-free function so the mix / delta / bypass seam can be unit-tested off
    the audio thread (test/spectral_ui_map_test.cpp) — the family habit of
    extracting the load-bearing math into a testable header (cf. comp/ui maps).

    The node delays the wet path by exactly N samples; the processor delays the
    dry path by a matching N-sample ring, so `dry` and `wet` here are already
    time-aligned. Given that, per sample:

      BYPASS : out = dry
               A transparent, latency-preserving passthrough — ignores mix,
               output trim AND wet trim (a bypass must not change level). The
               node keeps running upstream so un-bypassing never clicks; only the
               output selection changes.

      DELTA  : out = (wet*wetGain - dry) * outGain
               soothe's signature "listen to what I'm removing". wet is the
               resonance-suppressed signal, dry the original, so (wet - dry) is
               the difference the processor introduced — i.e. the removed
               component, up to overall polarity (a sign flip is inaudible).
               DELTA ignores mix by design (you audition the full effect).
               KEY PROPERTY: depth == 0 makes the node a bit-near identity, so
               (at wetGain == 1) wet == dry and DELTA collapses to exact silence
               — a bypassed processor soloes to nothing. The seam test pins this.

      NORMAL : out = ((1 - mix) * dry + mix * wet*wetGain) * outGain
               ordinary latency-aligned dry/wet blend (mix in [0,1]).

    WET TRIM (`wetGainLin` = 10^(wetTrimDb/20)) is the sidebar's flat gain on the
    fully-wet signal (proto/spectral_sidebar.py run_sidebar): it scales the WET
    component ONLY, in Normal and Delta, and is applied HERE rather than in the
    node so the node's identity/latency invariants are untouched and dry is never
    coloured. Bypass ignores it (level-preserving passthrough). Default 0 dB =>
    wetGainLin == 1 => the pre-sidebar behaviour, exactly.
*/
namespace spectralmix {

enum class Mode { Normal, Delta, Bypass, ScListen };

/** One output sample. `mixFrac` in [0,1], `outGainLin` = 10^(outDb/20),
    `wetGainLin` = 10^(wetTrimDb/20) (scales the wet component only). */
inline double blend(double dry, double wet, double mixFrac,
                    double outGainLin, double wetGainLin, Mode mode, double key = 0.0) {
    switch (mode) {
        case Mode::Bypass:
            return dry;
        case Mode::Delta:
            return (wet * wetGainLin - dry) * outGainLin;
        case Mode::ScListen:
            return key * outGainLin;
        case Mode::Normal:
        default:
            return ((1.0 - mixFrac) * dry + mixFrac * wet * wetGainLin) * outGainLin;
    }
}

// Verified spectral_sidechain.py route-weight ramp. Retarget from the current
// vector, advance once per sample (shared by both channels), including bypass.
class RouteCrossfade {
public:
    void prepare(double sampleRate, Mode initial) noexcept {
        length_ = std::max(1, static_cast<int>(std::floor(.005*sampleRate+.5)));
        selected_ = initial;
        weights_.fill(0.0);
        weights_[static_cast<std::size_t>(initial)] = 1.0;
        target_ = weights_;
        step_.fill(0.0);
        remaining_ = 0;
    }
    void setMode(Mode mode) noexcept {
        if (mode == selected_) return;
        selected_ = mode;
        target_.fill(0.0);
        target_[static_cast<std::size_t>(mode)] = 1.0;
        for (std::size_t i = 0; i < weights_.size(); ++i)
            step_[i] = (target_[i]-weights_[i])/length_;
        remaining_ = length_;
    }
    void advance() noexcept {
        if (remaining_ == 0) return;
        for (std::size_t i = 0; i < weights_.size(); ++i) weights_[i] += step_[i];
        if (--remaining_ == 0) weights_ = target_;
    }
    double blend(double dry, double wet, double key, double mix,
                 double outputGain, double wetGain) const noexcept {
        double result = 0.0;
        for (std::size_t i = 0; i < weights_.size(); ++i)
            if (weights_[i] != 0.0)
                result += weights_[i]*spectralmix::blend(dry, wet, mix, outputGain,
                                                         wetGain, static_cast<Mode>(i), key);
        return result;
    }
private:
    std::array<double, 4> weights_{1.0, 0.0, 0.0, 0.0}, target_{}, step_{};
    Mode selected_ = Mode::Normal;
    int length_ = 1, remaining_ = 0;
};

}  // namespace spectralmix
