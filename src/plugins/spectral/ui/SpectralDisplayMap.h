#pragma once

#include <cmath>

#include "dsp/SpectralWeight.h"

/**
    JUCE-free coordinate math for the LpngSpectral display, kept in its own
    header so the seams can be unit-tested with a bare console target
    (test/spectral_ui_map_test.cpp) — same discipline as comp/ui's CompCurveMap
    / CompMeterMap.

    FOUR DIFFERENT PHYSICAL QUANTITIES share this one plot rectangle, and — per
    the analyzer lesson (src/plugins/CLAUDE.md, PROJECT.md 이음새 버그) — each
    gets its OWN scale. Mixing them (e.g. drawing reduction dB on the spectrum's
    level axis) is exactly the class of bug that produced a blank analyzer while
    every component test passed:

      * frequency  -> x : log, 20 Hz .. 20 kHz (the parameter range).
      * spectrum level (dBFS-ish) -> y : linear, -90 .. 0 dB, bottom-up. This is
        the SAME kind of window AnalyzerView uses, and deliberately NOT any gain
        axis.
      * reduction  -> ribbon extent : a POSITIVE amount of cut, 0 .. kMaxRibbonDb,
        drawn as pixels hanging DOWN from a mid-well BASELINE (soothe3-style —
        the reduction reads against a reference line at ~36% of the well height,
        visually connected to the spectrum below, instead of dangling off the top
        edge). Its own scale; 0 dB -> at the baseline, kMaxRibbonDb -> baseline +
        the ribbon cap (kRibbonCapFrac of the well height).
      * weight curve (the soothe3-style band-editing UI, WeightBandLayer.h) -> y :
        linear, ±spectralweight::kWeightDisplayDb around the SAME baseline as the
        ribbon, spanning ±kWeightSpanFrac of the well height each way. Its own
        window too — a weight-curve dB and a reduction dB are not the same axis
        even though they share a baseline.

    Delta mode is NOT a fifth scale: it draws the SAME per-bin reduction data as
    the ribbon, on the reduction scale above (ribbonExtentPx), just mirrored
    UPWARD from the baseline instead of hanging down. So there is no dedicated
    delta y-map here — SpectralAnalyzerView negates the ribbon extent. Keeping
    delta on the reduction scale is what makes the excursion pixel-identical
    between delta ON and delta OFF for a given cut.
*/
namespace spectralui {

struct DisplayMap {
    static constexpr float kMinFreq = 20.0f;
    static constexpr float kMaxFreq = 20000.0f;

    // Spectrum level window (matches AnalyzerView's separate analyzer scale).
    static constexpr float kSpecMaxDb = 0.0f;
    static constexpr float kSpecMinDb = -90.0f;

    // Reduction that fills the ribbon to its cap. The node's ceiling at
    // depth10/detail5 is ~32 dB; 24 dB keeps the common working range legible
    // while extreme cuts simply saturate the ribbon.
    static constexpr float kMaxRibbonDb = 24.0f;

    // Reduction BASELINE: a horizontal reference line at this fraction of the
    // well height from the top. -30 dBFS on the spectrum axis lands at 33% from
    // the top, so a baseline at 36% sits just above the spectrum's usual
    // territory — the ribbon hangs into the empty upper band, not over the
    // signal. The cap (max ribbon depth) is kRibbonCapFrac of the well height,
    // so baseline + cap = 56% from top (~-40 dBFS line) — reduction stays clear
    // of typical spectrum peaks.
    static constexpr float kBaselineFrac = 0.36f;
    static constexpr float kRibbonCapFrac = 0.20f;

    // Weight-curve window: fraction of the well height that the
    // ±spectralweight::kWeightDisplayDb (24 dB) window spans EACH WAY from the
    // SAME baseline the ribbon hangs from (WeightBandLayer.h). Deliberately a
    // touch larger than kRibbonCapFrac (0.20) so a full-scale weight excursion
    // stays visually distinct from a full-scale ribbon cut, even though both
    // share the one reference line.
    static constexpr float kWeightSpanFrac = 0.30f;

    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

    void setBounds(float X, float Y, float W, float H) { x = X; y = Y; w = W; h = H; }

    /** y of the reduction baseline (0 dB reduction). */
    float baselineY() const { return y + kBaselineFrac * h; }

    /** Ribbon pixel cap: max downward extent of a full-scale (>= kMaxRibbonDb) cut. */
    float ribbonCapPx() const { return kRibbonCapFrac * h; }

    /** Weight-curve dB -> y, anchored at the SAME baseline as the ribbon: 0 dB
        on the baseline, +kWeightDisplayDb (24) up by kWeightSpanFrac of the well
        height, -kWeightDisplayDb down by the same amount, linear between.
        Clamped to ±kWeightDisplayDb (WeightBandLayer.h). */
    float weightDbToY(float db) const {
        const auto maxDb = static_cast<float>(spectralweight::kWeightDisplayDb);
        const float dc = db < -maxDb ? -maxDb : (db > maxDb ? maxDb : db);
        return baselineY() - (dc / maxDb) * (kWeightSpanFrac * h);
    }

    /** Exact inverse of weightDbToY, clamped to the same ±kWeightDisplayDb window. */
    float yToWeightDb(float ypx) const {
        const auto maxDb = static_cast<float>(spectralweight::kWeightDisplayDb);
        const float span = kWeightSpanFrac * h;
        float t = (baselineY() - ypx) / span;
        t = t < -1.0f ? -1.0f : (t > 1.0f ? 1.0f : t);
        return t * maxDb;
    }

    float freqToX(float f) const {
        const float logMin = std::log10(kMinFreq);
        const float logMax = std::log10(kMaxFreq);
        const float fc = f < kMinFreq ? kMinFreq : (f > kMaxFreq ? kMaxFreq : f);
        const float t = (std::log10(fc) - logMin) / (logMax - logMin);
        return x + t * w;
    }

    float xToFreq(float px) const {
        const float logMin = std::log10(kMinFreq);
        const float logMax = std::log10(kMaxFreq);
        float t = (px - x) / w;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return std::pow(10.0f, logMin + t * (logMax - logMin));
    }

    /** Spectrum level dB -> y (bottom at kSpecMinDb, top at kSpecMaxDb). */
    float specDbToY(float db) const {
        const float dc = db < kSpecMinDb ? kSpecMinDb : (db > kSpecMaxDb ? kSpecMaxDb : db);
        const float t = (dc - kSpecMinDb) / (kSpecMaxDb - kSpecMinDb);
        return (y + h) - t * h;  // dB up -> y up (screen y grows down)
    }

    /** Reduction dB -> ribbon pixel extent hanging from the baseline. 0 dB -> 0 px,
        >= kMaxRibbonDb -> ribbonHeightPx (clamped). Static: independent of the
        plot bounds, driven only by the caller's chosen ribbon cap height. */
    static float ribbonExtentPx(float redDb, float ribbonHeightPx) {
        float t = redDb / kMaxRibbonDb;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return t * ribbonHeightPx;
    }

    /** Display-only STARVATION DECAY (not DSP): one exponential step of a shown
        value toward a floor. When no new analyzer frame arrives (host stopped
        calling processBlock — e.g. Logic's power-button bypass), the view pulls
        the frozen spectrum toward kSpecMinDb and the ribbon toward 0 instead of
        holding the last frame forever. `coeff` is exp(-1/(rateHz*tau)); after k
        starved ticks a value sits at floor + coeff^k*(start-floor). Same idiom
        as comp/ui LevelMeter's release ballistics. */
    static float decayTowardFloor(float current, float floor, float coeff) {
        return floor + coeff * (current - floor);
    }
};

}  // namespace spectralui
