#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "dsp/SpectralNode.h"
#include "plugins/LpngLookAndFeel.h"
#include "plugins/eq/SpectrumFifo.h"
#include "plugins/eq/ui/AnalyzerEngine.h"
#include "plugins/spectral/ReductionPublisher.h"
#include "plugins/spectral/ui/SpectralDisplayMap.h"

/**
    The main display well: a live INPUT spectrum (deep-azure fill + line) with the
    ember REDUCTION RIBBON hanging from a mid-well BASELINE — soothe3-style: a
    horizontal reference line at ~36% of the well height, reduction hanging
    DOWNWARD from it (the further a band hangs down, the more that region is
    being cut), visually connected to the spectrum below rather than dangling off
    the top edge.

    STARVATION DECAY (display behaviour, NOT DSP): if the specFifo delivers no
    new frames for kStarveGraceTicks (the host stopped calling processBlock —
    e.g. Logic's own power-button bypass), the shown spectrum decays toward the
    floor and the ribbon toward zero, instead of freezing on the last frame. Our
    OWN bypass pill never triggers this: processBlock still taps specFifo and
    runs the node under our bypass, so the analyzer stays live there.

    Reuses the EQ's proven AnalyzerEngine + SpectrumFifo (falling-spectrum, 30 Hz)
    directly; the coupling to plugins/eq/ui is deliberate for now and flagged as a
    future shared-ui refactor item (a plain `#include`, no EQ state pulled in).

    TWO SEPARATE SCALES on one rectangle (the analyzer lesson, applied again):
    the spectrum is drawn on DisplayMap's spectrum-LEVEL window (-90..0 dBFS-ish),
    the ribbon on DisplayMap's REDUCTION window (0..kMaxRibbonDb of cut). They are
    different physical quantities and never share a y-axis. Frequency (x, log
    100..16k) is the one shared axis. All the coordinate math lives in the
    JUCE-free DisplayMap so it is unit-tested (test/spectral_ui_map_test.cpp).

    Threading: its own 30 Hz timer drains the processor's specFifo and reads the
    reduction seqlock on the message thread; paint() only fills/strokes prebuilt
    paths.
*/
class SpectralAnalyzerView : public juce::Component, private juce::Timer {
public:
    static constexpr int kNumPoints = 512;

    SpectralAnalyzerView(SpectrumFifo& fifoIn,
                         ReductionPublisher<SpectralNode::kNumBins>& reductionIn,
                         const std::atomic<double>& hostFs)
        : fifo(fifoIn), reduction(reductionIn), hostSampleRate(hostFs) {
        setInterceptsMouseClicks(false, false);

        const float logMin = std::log10(spectralui::DisplayMap::kMinFreq);
        const float logMax = std::log10(spectralui::DisplayMap::kMaxFreq);
        for (int p = 0; p < kNumPoints; ++p) {
            const float t = static_cast<float>(p) / static_cast<float>(kNumPoints - 1);
            freqPoints[static_cast<std::size_t>(p)] = std::pow(10.0f, logMin + t * (logMax - logMin));
        }

        for (auto& b : scratch)
            b.assign(static_cast<std::size_t>(kScratchCap), 0.0f);
        scratchPtr[0] = scratch[0].data();
        scratchPtr[1] = scratch[1].data();

        reductionDb.fill(0.0f);
        displaySpecDb.fill(spectralui::DisplayMap::kSpecMinDb);
        engine.setSampleRate(readHostFs());
        startTimerHz(30);
    }

    void resized() override {
        auto b = getLocalBounds().toFloat().reduced(static_cast<float>(kInset));
        map.setBounds(b.getX(), b.getY(), b.getWidth(), b.getHeight());
        rebuildPaths();
        repaint();
    }

    void paint(juce::Graphics& g) override {
        namespace pl = lpng::palettelight;
        auto well = getLocalBounds().toFloat();

        // White well with a stone hairline inset (no shadow — paper).
        g.setColour(pl::col(pl::displayWell));
        g.fillRoundedRectangle(well, lpng::metrics::kCornerCard);
        g.setColour(pl::col(pl::hairline));
        g.drawRoundedRectangle(well.reduced(0.5f), lpng::metrics::kCornerCard, 1.0f);

        drawGrid(g);
        drawReductionBaseline(g);

        // Input spectrum: azure translucent fill + azure line.
        g.setColour(pl::col(pl::dataPost).withAlpha(0.11f));
        g.fillPath(specFillPath);
        g.setColour(pl::col(pl::dataPost));
        g.strokePath(specLinePath, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));

        // Reduction ribbon: ember region hanging from the top + ember contour.
        g.setColour(pl::col(pl::reduceFill));
        g.fillPath(ribbonFillPath);
        g.setColour(pl::col(pl::accent));
        g.strokePath(ribbonLinePath, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));

        drawAxisText(g);
    }

private:
    static constexpr int kInset = 6;
    static constexpr int kScratchCap = 1 << 15;

    static inline const std::array<float, 8> kGridFreqs{100, 250, 500, 1000, 2000, 4000, 8000, 16000};
    static inline const std::array<const char*, 8> kGridLabels{"100", "250", "500", "1k", "2k", "4k", "8k", "16k"};
    static inline const std::array<float, 4> kDbMarks{0.0f, -30.0f, -60.0f, -90.0f};
    // Reduction-scale ticks (dB of cut), drawn near the baseline on the LEFT so
    // they never collide with the spectrum dBFS numerals on the right.
    static inline const std::array<float, 3> kRibbonMarks{0.0f, 8.0f, 16.0f};

    // Starvation decay (display ballistics, mirrors comp/ui LevelMeter): begin
    // decaying after ~250 ms of no frames; tau ~400 ms at the 30 Hz timer.
    static constexpr int kStarveGraceTicks = 8;              // ~0.27 s at 30 Hz
    static constexpr float kDecayCoeff = 0.92004f;           // exp(-1/(30*0.4))

    double readHostFs() const {
        const double fs = hostSampleRate.load(std::memory_order_relaxed);
        return fs > 0.0 ? fs : 48000.0;
    }

    void timerCallback() override {
        engine.setSampleRate(readHostFs());
        const int n = fifo.read(scratchPtr, 2, kScratchCap);

        if (n > 0) {
            // Live: fresh audio is flowing (processBlock is being called, so the
            // reduction snapshot is fresh too). Resample the engine's spectrum
            // into the held display array and take the latest reduction frame.
            starveTicks = 0;
            engine.pushBlock(scratchPtr, fifo.getNumChannels(), n);
            engine.computeFrame();
            resampleLiveSpectrum();

            std::array<float, SpectralNode::kNumBins> snap{};
            if (reduction.read(snap.data(), SpectralNode::kNumBins))
                reductionDb = snap;
        } else if (++starveTicks > kStarveGraceTicks) {
            // Starved past the grace window: the host stopped feeding us (its own
            // bypass / power button). Decay the frozen frame toward the floor so
            // the view doesn't hold a stale spectrum forever. DISPLAY-only.
            for (auto& d : displaySpecDb)
                d = spectralui::DisplayMap::decayTowardFloor(
                    d, spectralui::DisplayMap::kSpecMinDb, kDecayCoeff);
            for (auto& r : reductionDb)
                r = spectralui::DisplayMap::decayTowardFloor(r, 0.0f, kDecayCoeff);
        }
        // Within the grace window we simply hold, so a single dropped block
        // (UI momentarily behind) never flickers the display.

        rebuildPaths();
        repaint();
    }

    void rebuildPaths() {
        buildSpectrum();
        buildRibbon();
    }

    // Resample the engine's per-bin dB into the per-point display array. Split
    // out of path-building so the timer can decay this held copy while starved.
    void resampleLiveSpectrum() {
        const auto& db = engine.binDb();
        const double fs = engine.getSampleRate() > 0.0 ? engine.getSampleRate() : 48000.0;
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
            displaySpecDb[static_cast<std::size_t>(p)] = dbv;
        }
    }

    void buildSpectrum() {
        specLinePath.clear();
        specFillPath.clear();
        const float bottom = map.specDbToY(spectralui::DisplayMap::kSpecMinDb);

        float firstX = 0.0f, lastX = 0.0f;
        for (int p = 0; p < kNumPoints; ++p) {
            const float f = freqPoints[static_cast<std::size_t>(p)];
            const float x = map.freqToX(f);
            const float y = map.specDbToY(displaySpecDb[static_cast<std::size_t>(p)]);
            if (p == 0) { firstX = x; specLinePath.startNewSubPath(x, y); }
            else        { specLinePath.lineTo(x, y); }
            lastX = x;
        }
        specFillPath = specLinePath;
        specFillPath.lineTo(lastX, bottom);
        specFillPath.lineTo(firstX, bottom);
        specFillPath.closeSubPath();
    }

    void buildRibbon() {
        ribbonLinePath.clear();
        ribbonFillPath.clear();
        // Ribbon hangs DOWN from the mid-well baseline; the cap bounds a
        // full-scale cut to a legible depth that stays above spectrum territory.
        const float base = map.baselineY();
        const float ribbonBand = map.ribbonCapPx();
        const double fs = readHostFs();

        float firstX = 0.0f, lastX = 0.0f;
        bool any = false;
        for (int p = 0; p < kNumPoints; ++p) {
            const float f = freqPoints[static_cast<std::size_t>(p)];
            // Node reduction bins: f -> bin index at the node's N (1024).
            const double binF = static_cast<double>(f) * SpectralNode::kFftSize / fs;
            float redv;
            const int k0 = static_cast<int>(std::floor(binF));
            if (k0 < 0)
                redv = reductionDb.front();
            else if (k0 >= SpectralNode::kNumBins - 1)
                redv = reductionDb.back();
            else {
                const float t = static_cast<float>(binF - k0);
                redv = reductionDb[static_cast<std::size_t>(k0)] * (1.0f - t) +
                       reductionDb[static_cast<std::size_t>(k0 + 1)] * t;
            }
            const float x = map.freqToX(f);
            const float y = base + spectralui::DisplayMap::ribbonExtentPx(redv, ribbonBand);
            if (redv > 0.01f) any = true;
            if (p == 0) { firstX = x; ribbonLinePath.startNewSubPath(x, y); }
            else        { ribbonLinePath.lineTo(x, y); }
            lastX = x;
        }
        if (!any) { ribbonLinePath.clear(); return; }  // nothing cut -> no ribbon
        ribbonFillPath = ribbonLinePath;
        ribbonFillPath.lineTo(lastX, base);
        ribbonFillPath.lineTo(firstX, base);
        ribbonFillPath.closeSubPath();
    }

    void drawGrid(juce::Graphics& g) {
        namespace pl = lpng::palettelight;
        for (std::size_t i = 0; i < kGridFreqs.size(); ++i) {
            const float x = map.freqToX(kGridFreqs[i]);
            g.setColour(pl::col(kGridFreqs[i] == 1000.0f ? pl::gridMajor : pl::gridMinor));
            g.drawVerticalLine(static_cast<int>(x), map.y, map.y + map.h);
        }
        for (float db : kDbMarks) {
            const float y = map.specDbToY(db);
            g.setColour(pl::col(pl::gridMinor));
            g.drawHorizontalLine(static_cast<int>(y), map.x, map.x + map.w);
        }
    }

    // Reduction reference line + its own dB scale (0/-8/-16), kept visually
    // distinct from the spectrum's dBFS numerals (the analyzer lesson: two
    // vertical scales, never mixed). Ember-tinted so it reads as the action axis.
    void drawReductionBaseline(juce::Graphics& g) {
        namespace pl = lpng::palettelight;
        const float base = map.baselineY();
        const float cap = map.ribbonCapPx();

        g.setColour(pl::col(pl::tick));
        g.drawHorizontalLine(static_cast<int>(base), map.x, map.x + map.w);

        g.setFont(lpng::labelFont(lpng::metrics::kFontSmall));
        for (float redDb : kRibbonMarks) {
            const float y = base + spectralui::DisplayMap::ribbonExtentPx(redDb, cap);
            g.setColour(pl::col(pl::accentDim));
            g.drawText(redDb == 0.0f ? juce::String("0") : juce::String("-" + juce::String(static_cast<int>(redDb))),
                       static_cast<int>(map.x) + 4, static_cast<int>(y) - 7,
                       26, 14, juce::Justification::centredLeft);
        }
    }

    void drawAxisText(juce::Graphics& g) {
        namespace pl = lpng::palettelight;
        g.setFont(lpng::labelFont(lpng::metrics::kFontSmall));
        // Frequency numerals along the top.
        g.setColour(pl::col(pl::textMuted));
        for (std::size_t i = 0; i < kGridFreqs.size(); ++i) {
            const float x = map.freqToX(kGridFreqs[i]);
            g.drawText(kGridLabels[i], static_cast<int>(x) - 16, static_cast<int>(map.y) + 2,
                       32, 12, juce::Justification::centred);
        }
        // Spectrum level numerals on the right edge (faint — the analyzer's own
        // window, deliberately separate from the reduction scale).
        g.setColour(pl::col(pl::textFaint));
        for (float db : kDbMarks) {
            const float y = map.specDbToY(db);
            g.drawText(juce::String(static_cast<int>(db)),
                       static_cast<int>(map.x + map.w) - 30, static_cast<int>(y) - 7,
                       28, 14, juce::Justification::centredRight);
        }
    }

    SpectrumFifo& fifo;
    ReductionPublisher<SpectralNode::kNumBins>& reduction;
    const std::atomic<double>& hostSampleRate;

    spectralui::DisplayMap map;
    AnalyzerEngine engine;

    std::array<float, kNumPoints> freqPoints{};
    std::array<std::vector<float>, 2> scratch{};
    float* scratchPtr[2]{nullptr, nullptr};
    std::array<float, SpectralNode::kNumBins> reductionDb{};
    std::array<float, kNumPoints> displaySpecDb{};  // held per-point spectrum (decays when starved)
    int starveTicks = 0;

    juce::Path specLinePath, specFillPath;
    juce::Path ribbonLinePath, ribbonFillPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralAnalyzerView)
};
