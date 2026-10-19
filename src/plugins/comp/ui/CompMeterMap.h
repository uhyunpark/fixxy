#pragma once

/**
    Meter scale mappers for the fixxy Comp editor — deliberately TWO separate
    mappers because the two meter families measure different physical
    quantities on different scales (the analyzer lesson, src/plugins/AGENTS.md,
    applied to meters):

      * IN/OUT level meters read dBFS peaks: NEGATIVE numbers, -60..0, and the
        bar fills bottom-up (louder = taller).
      * The GR meter reads gain-reduction MAGNITUDE: a POSITIVE number of dB
        (the processor publishes |GR|), displayed on its own 0..-30 scale drawn
        TOP-DOWN (more reduction = the bar reaches further down from the top —
        the visual convention every hardware GR meter uses).

    Feeding one quantity through the other's mapper would clamp everything to
    an edge and produce a meter that is always empty/always full while every
    per-component test still passes — hence one tiny JUCE-free header, unit
    tested in test/comp_ui_map_test.cpp.

    Both return a normalized fill fraction in [0..1]; the component turns that
    into pixels against its own bar rectangle:
      level: fraction = 0 -> empty (bar bottom),  1 -> full (bar top)
      GR:    fraction = 0 -> empty (nothing off the top), 1 -> full bar
*/
namespace compui {

/** IN/OUT peak meters: dBFS window -60..0. */
struct LevelMeterMap {
    static constexpr float kMinDb = -60.0f;
    static constexpr float kMaxDb = 0.0f;

    /** dBFS -> fill fraction [0..1] (0 at/below -60, 1 at/above 0). */
    static float fraction(float db) {
        const float t = (db - kMinDb) / (kMaxDb - kMinDb);
        return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
};

/** GR meter: gain-reduction magnitude window 0..30 dB (labelled 0..-30,
    drawn top-down). Input is the POSITIVE magnitude the processor publishes. */
struct GrMeterMap {
    static constexpr float kMaxGrDb = 30.0f;

    /** |GR| dB -> fill fraction [0..1] (0 at no reduction, 1 at >= 30 dB). */
    static float fraction(float grDb) {
        const float t = grDb / kMaxGrDb;
        return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
};

}  // namespace compui
