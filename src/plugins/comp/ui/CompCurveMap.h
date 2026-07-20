#pragma once

/**
    Coordinate mapping for the transfer-curve display: a SQUARE in-dB x out-dB
    plane, -60..0 dB on BOTH axes, linear on both (deliberately separate from
    the EQ's CoordMap, which is log-frequency x gain — different axes, different
    quantities). Input dB runs left->right, output dB runs bottom->top (screen y
    grows down). One struct so the grid, the curve, the threshold marker and the
    live IO dot can never disagree about where a dB value lands.

    JUCE-free (plain floats) so the round-trip unit test
    (test/comp_ui_map_test.cpp) needs no JUCE; the view hands in its bounds
    rectangle as x/y/w/h.
*/
namespace compui {

struct CompCurveMap {
    static constexpr float kMinDb = -60.0f;
    static constexpr float kMaxDb = 0.0f;

    float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f;  // pixel bounds of the plot

    void setBounds(float xIn, float yIn, float wIn, float hIn) {
        x = xIn;
        y = yIn;
        w = wIn;
        h = hIn;
    }

    static float clampDb(float db) {
        return db < kMinDb ? kMinDb : (db > kMaxDb ? kMaxDb : db);
    }

    /** Input level (dB) -> screen x. -60 at the left edge, 0 at the right. */
    float dbToX(float inDb) const {
        const float t = (clampDb(inDb) - kMinDb) / (kMaxDb - kMinDb);
        return x + t * w;
    }

    /** Output level (dB) -> screen y. -60 at the bottom edge, 0 at the top. */
    float dbToY(float outDb) const {
        const float t = (clampDb(outDb) - kMinDb) / (kMaxDb - kMinDb);
        return y + h - t * h;
    }

    float xToDb(float px) const {
        float t = (px - x) / w;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return kMinDb + t * (kMaxDb - kMinDb);
    }

    float yToDb(float py) const {
        float t = (y + h - py) / h;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return kMinDb + t * (kMaxDb - kMinDb);
    }
};

}  // namespace compui
