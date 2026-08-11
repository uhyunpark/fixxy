#pragma once

/**
    LpngSpectral output-stage algebra, factored out of processBlock() as a pure,
    JUCE-free function so the mix / delta / bypass seam can be unit-tested off
    the audio thread (test/spectral_ui_map_test.cpp) — the family habit of
    extracting the load-bearing math into a testable header (cf. comp/ui maps).

    The node delays the wet path by exactly N samples; the processor delays the
    dry path by a matching N-sample ring, so `dry` and `wet` here are already
    time-aligned. Given that, per sample:

      BYPASS : out = dry
               A transparent, latency-preserving passthrough — ignores mix AND
               output trim (a bypass must not change level). The node keeps
               running upstream so un-bypassing never clicks; only the output
               selection changes.

      DELTA  : out = (wet - dry) * outGain
               soothe's signature "listen to what I'm removing". wet is the
               resonance-suppressed signal, dry the original, so (wet - dry) is
               the difference the processor introduced — i.e. the removed
               component, up to overall polarity (a sign flip is inaudible).
               DELTA ignores mix by design (you audition the full effect).
               KEY PROPERTY: depth == 0 makes the node a bit-near identity, so
               wet == dry and DELTA collapses to exact silence — a bypassed
               processor soloes to nothing. The seam test pins this.

      NORMAL : out = ((1 - mix) * dry + mix * wet) * outGain
               ordinary latency-aligned dry/wet blend (mix in [0,1]).
*/
namespace spectralmix {

enum class Mode { Normal, Delta, Bypass };

/** One output sample. `mixFrac` in [0,1], `outGainLin` = 10^(outDb/20). */
inline double blend(double dry, double wet, double mixFrac, double outGainLin, Mode mode) {
    switch (mode) {
        case Mode::Bypass:
            return dry;
        case Mode::Delta:
            return (wet - dry) * outGainLin;
        case Mode::Normal:
        default:
            return ((1.0 - mixFrac) * dry + mixFrac * wet) * outGainLin;
    }
}

}  // namespace spectralmix
