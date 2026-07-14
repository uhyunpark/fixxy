#pragma once

#include <cmath>

#include <juce_graphics/juce_graphics.h>

/**
    Shared freq<->x (log) and dB<->y (linear) mapping for every layer of the
    curve display (background grid, per-band paths, sum path). One struct so
    the grid lines and the curves painted over them can never disagree about
    where a frequency or a dB value lands.
*/
struct CoordMap {
    static constexpr float kMinFreq = 20.0f;
    static constexpr float kMaxFreq = 20000.0f;
    static constexpr float kMinDb = -30.0f;
    static constexpr float kMaxDb = 30.0f;

    juce::Rectangle<float> bounds;

    void setBounds(juce::Rectangle<float> b) { bounds = b; }

    float freqToX(float freq) const {
        const float logMin = std::log10(kMinFreq);
        const float logMax = std::log10(kMaxFreq);
        const float t = (std::log10(juce::jlimit(kMinFreq, kMaxFreq, freq)) - logMin) /
                        (logMax - logMin);
        return bounds.getX() + t * bounds.getWidth();
    }

    float xToFreq(float x) const {
        const float logMin = std::log10(kMinFreq);
        const float logMax = std::log10(kMaxFreq);
        const float t = juce::jlimit(0.0f, 1.0f, (x - bounds.getX()) / bounds.getWidth());
        return std::pow(10.0f, logMin + t * (logMax - logMin));
    }

    float dbToY(float db) const {
        const float t = (juce::jlimit(kMinDb, kMaxDb, db) - kMinDb) / (kMaxDb - kMinDb);
        // dB increases upward -> y decreases upward (screen y grows down).
        return bounds.getBottom() - t * bounds.getHeight();
    }

    float yToDb(float y) const {
        const float t = juce::jlimit(0.0f, 1.0f, (bounds.getBottom() - y) / bounds.getHeight());
        return kMinDb + t * (kMaxDb - kMinDb);
    }
};
