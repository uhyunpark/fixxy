#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "AnalyzerEngine.h"
#include "CoordMap.h"
#include "plugins/eq/SpectrumFifo.h"

/**
    Falling pre/post spectrum drawn BEHIND the response curve. Owns two
    AnalyzerEngines (pre-EQ, post-EQ) and, on its own 30 Hz timer, drains each
    processor FIFO on the message thread, computes a frame, and rebuilds the
    display paths. paint() only fills/strokes prebuilt paths — no DSP, no math.

    Layered under CurveView (added earlier in the editor) and transparent to the
    mouse, so band interaction still hits the curve.
*/
class AnalyzerView : public juce::Component, private juce::Timer {
public:
    static constexpr int kNumPoints = 512;

    AnalyzerView(SpectrumFifo& preFifoIn, SpectrumFifo& postFifoIn, CoordMap& mapIn,
                 const std::atomic<double>& hostFs)
        : preFifo(preFifoIn), postFifo(postFifoIn), map(mapIn), hostSampleRate(hostFs) {
        setInterceptsMouseClicks(false, false);

        const float logMin = std::log10(CoordMap::kMinFreq);
        const float logMax = std::log10(CoordMap::kMaxFreq);
        for (int p = 0; p < kNumPoints; ++p) {
            const float t = static_cast<float>(p) / static_cast<float>(kNumPoints - 1);
            freqPoints[static_cast<std::size_t>(p)] = std::pow(10.0f, logMin + t * (logMax - logMin));
        }

        for (auto& b : scratch)
            b.assign(static_cast<std::size_t>(kScratchCap), 0.0f);
        scratchPtr[0] = scratch[0].data();
        scratchPtr[1] = scratch[1].data();

        const double fs = readHostFs();
        preEngine.setSampleRate(fs);
        postEngine.setSampleRate(fs);

        startTimerHz(30);
    }

    void resized() override {
        // map bounds already updated by BackgroundPanel; re-stroke from cache.
        rebuildPaths();
        repaint();
    }

    void paint(juce::Graphics& g) override {
        // Pre-EQ: translucent fill only (the "before" reference sits behind).
        g.setColour(kPreColour.withAlpha(0.16f));
        g.fillPath(preFillPath);

        // Post-EQ: lighter fill + stroked outline (what the EQ is doing now).
        g.setColour(kPostColour.withAlpha(0.10f));
        g.fillPath(postFillPath);
        g.setColour(kPostColour.withAlpha(0.85f));
        g.strokePath(postLinePath, juce::PathStrokeType(1.3f));

        // P1-1: right-edge dB labels for the analyzer's own -90..0 window.
        // Deliberately on the opposite edge and dimmer than BackgroundPanel's
        // left-edge +-24 EQ-gain labels -- two different axes sharing one
        // rect would be confusing if they looked like one scale. No
        // gridlines: the frequency/gain grid underneath already does that job.
        g.setFont(juce::FontOptions(10.0f));
        g.setColour(juce::Colour(0xff606068));
        for (const float db : {0.0f, -30.0f, -60.0f, -90.0f}) {
            const float y = dbToAnalyzerY(db);
            g.drawText(juce::String(static_cast<int>(db)),
                       static_cast<int>(map.bounds.getRight()) - 34, static_cast<int>(y) - 7, 30, 14,
                       juce::Justification::centredRight);
        }
    }

private:
    static inline const juce::Colour kPreColour{0xff6b7a90};
    static inline const juce::Colour kPostColour{0xff8fd0ff};
    static constexpr int kScratchCap = 1 << 15;  // >= any FIFO capacity

    // Analyzer-specific dB window, deliberately separate from CoordMap's
    // +-30 dB window (CoordMap::kMinDb/kMaxDb). The curve display's dB axis is
    // EQ *gain* (how much a band boosts/cuts); the analyzer's dB axis is
    // spectrum *level* (dBFS-ish signal energy per bin) -- a different physical
    // quantity with a different useful range. Program material sits around
    // -40..-90 dB per bin, nowhere near a +-30 dB gain window, so feeding one
    // through the other clamps every point to the bottom edge and the analyzer
    // never draws (this was the ANALYZER-SCALE bug -- see PROJECT.md 함정
    // "이음새 버그", docs/issues.md). Every commercial EQ analyzer uses two
    // separate scales for exactly this reason. Only the y-axis differs; the x
    // (frequency) axis still comes from the shared CoordMap so the analyzer and
    // the response curve always line up on frequency.
    static constexpr float kAnalyzerMaxDb = 0.0f;
    static constexpr float kAnalyzerMinDb = -90.0f;

    /** dB (spectrum level) -> y, using the analyzer's own window but the
        shared CoordMap's bounds rectangle (so it still sizes with the curve
        display). */
    float dbToAnalyzerY(float db) const {
        const float t = (juce::jlimit(kAnalyzerMinDb, kAnalyzerMaxDb, db) - kAnalyzerMinDb) /
                        (kAnalyzerMaxDb - kAnalyzerMinDb);
        return map.bounds.getBottom() - t * map.bounds.getHeight();
    }

    double readHostFs() const {
        const double fs = hostSampleRate.load(std::memory_order_relaxed);
        return fs > 0.0 ? fs : 48000.0;
    }

    void timerCallback() override {
        const double fs = readHostFs();
        preEngine.setSampleRate(fs);
        postEngine.setSampleRate(fs);

        drainInto(preFifo, preEngine);
        drainInto(postFifo, postEngine);
        preEngine.computeFrame();
        postEngine.computeFrame();

        rebuildPaths();
        repaint();
    }

    void drainInto(SpectrumFifo& fifo, AnalyzerEngine& engine) {
        const int n = fifo.read(scratchPtr, 2, kScratchCap);
        if (n > 0)
            engine.pushBlock(scratchPtr, fifo.getNumChannels(), n);
    }

    void rebuildPaths() {
        buildPaths(preEngine, preLinePath, preFillPath);
        buildPaths(postEngine, postLinePath, postFillPath);
    }

    void buildPaths(const AnalyzerEngine& engine, juce::Path& line, juce::Path& fill) {
        line.clear();
        fill.clear();
        const auto& db = engine.binDb();
        const double fs = engine.getSampleRate() > 0.0 ? engine.getSampleRate() : 48000.0;
        const float bottom = dbToAnalyzerY(kAnalyzerMinDb);

        float firstX = 0.0f, lastX = 0.0f;
        for (int p = 0; p < kNumPoints; ++p) {
            const float f = freqPoints[static_cast<std::size_t>(p)];
            const double binF = static_cast<double>(f) * AnalyzerEngine::kFftSize / fs;
            float dbv;
            const int k0 = static_cast<int>(std::floor(binF));
            if (k0 < 0)
                dbv = db.front();
            else if (k0 >= AnalyzerEngine::kNumBins - 1)
                dbv = db.back();
            else {
                const float t = static_cast<float>(binF - k0);
                dbv = db[static_cast<std::size_t>(k0)] * (1.0f - t) +
                      db[static_cast<std::size_t>(k0 + 1)] * t;
            }
            const float x = map.freqToX(f);
            const float y = dbToAnalyzerY(dbv);
            if (p == 0) {
                firstX = x;
                line.startNewSubPath(x, y);
            } else {
                line.lineTo(x, y);
            }
            lastX = x;
        }

        fill = line;
        fill.lineTo(lastX, bottom);
        fill.lineTo(firstX, bottom);
        fill.closeSubPath();
    }

    SpectrumFifo& preFifo;
    SpectrumFifo& postFifo;
    CoordMap& map;
    const std::atomic<double>& hostSampleRate;

    AnalyzerEngine preEngine;
    AnalyzerEngine postEngine;

    std::array<float, kNumPoints> freqPoints{};
    std::array<std::vector<float>, 2> scratch{};
    float* scratchPtr[2]{nullptr, nullptr};

    juce::Path preLinePath, preFillPath;
    juce::Path postLinePath, postFillPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnalyzerView)
};
