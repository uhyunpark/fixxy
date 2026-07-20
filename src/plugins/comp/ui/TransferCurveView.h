#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "CompCurveMap.h"
#include "dsp/CompressorNode.h"
#include "plugins/LpngLookAndFeel.h"

/**
    Static transfer-curve display: in-dB vs out-dB (-60..0 square, linear —
    CompCurveMap), showing the gain computer's curve y_G(x) for the CURRENT
    APVTS threshold/ratio/knee (+range clamp when active).

    Single-source rule: the curve is evaluated through
    CompressorNode::staticCurve() — the SAME static function the audio kernel's
    gainComputer() delegates to — from APVTS values. The view never touches the
    audio-thread node instance, so there is no race and no way for the drawn
    curve to disagree with the running kernel.

    The curve represents the plugin's ACTION on the signal, so it is stroked in
    the warm accent (palette rule); grid/diagonal are monochrome chrome.

    Repaint discipline: the editor listens to threshold/ratio/knee/range and
    calls paramsChanged() via AsyncUpdater — the path is rebuilt ONLY then, not
    on a timer. The live IO dot lives in the separate IoDotLayer overlay, which
    repaints at the 30 Hz meter rate without re-tessellating this view.
*/
class TransferCurveView : public juce::Component {
public:
    explicit TransferCurveView(juce::AudioProcessorValueTreeState& state) : apvts(state) {
        threshold = apvts.getRawParameterValue("threshold");
        ratio = apvts.getRawParameterValue("ratio");
        knee = apvts.getRawParameterValue("knee");
        range = apvts.getRawParameterValue("range");
        setInterceptsMouseClicks(false, false);
    }

    /** The drawn static response, shared with IoDotLayer so the dot always sits
        ON the drawn curve: y_G(inDb) with the range clamp folded in when range
        is active (applied GR is limited to rangeDb, so out >= in - rangeDb). */
    static double curveOutDb(double inDb, double thresholdDb, double ratioV, double kneeDb,
                             double rangeDb) {
        double out = CompressorNode::staticCurve(inDb, thresholdDb, ratioV, kneeDb);
        if (rangeDb < CompressorNode::kRangeOffDb)
            out = std::max(out, inDb - rangeDb);
        return out;
    }

    /** Editor calls this (message thread, via AsyncUpdater) when T/R/W/range change. */
    void paramsChanged() {
        rebuildPath();
        repaint();
    }

    void resized() override {
        map.setBounds(0.0f, 0.0f, static_cast<float>(getWidth()),
                      static_cast<float>(getHeight()));
        rebuildPath();
    }

    const compui::CompCurveMap& getMap() const { return map; }

    void paint(juce::Graphics& g) override {
        using namespace lpng;
        const auto r = getLocalBounds().toFloat();

        g.setColour(palette::col(palette::displayWell));
        g.fillRoundedRectangle(r, metrics::kCorner);

        // Grid every 10 dB, both axes (chrome).
        g.setFont(labelFont(10.0f));
        for (int db = -50; db < 0; db += 10) {
            const float x = map.dbToX(static_cast<float>(db));
            const float y = map.dbToY(static_cast<float>(db));
            g.setColour(palette::col(db % 20 == 0 ? palette::gridMajor : palette::gridMinor));
            g.drawLine(x, r.getY(), x, r.getBottom(), 1.0f);
            g.drawLine(r.getX(), y, r.getRight(), y, 1.0f);
        }
        // Axis numerals every 20 dB: input along the bottom, output up the left.
        g.setColour(palette::col(palette::textMuted));
        for (int db = -40; db < 0; db += 20) {
            const float x = map.dbToX(static_cast<float>(db));
            const float y = map.dbToY(static_cast<float>(db));
            g.drawText(juce::String(db), static_cast<int>(x) - 12,
                       static_cast<int>(r.getBottom()) - 14, 24, 12,
                       juce::Justification::centred);
            g.drawText(juce::String(db), static_cast<int>(r.getX()) + 2,
                       static_cast<int>(y) - 6, 24, 12, juce::Justification::centredLeft);
        }

        // Unity diagonal (faint chrome reference: out == in, no compression).
        g.setColour(palette::col(palette::tick).withAlpha(0.5f));
        g.drawLine(map.dbToX(compui::CompCurveMap::kMinDb), map.dbToY(compui::CompCurveMap::kMinDb),
                   map.dbToX(compui::CompCurveMap::kMaxDb), map.dbToY(compui::CompCurveMap::kMaxDb),
                   1.0f);

        // Knee-width shading around threshold [T - W/2, T + W/2].
        const float t = threshold->load();
        const float w = knee->load();
        if (w > 0.01f) {
            const float x0 = map.dbToX(t - w * 0.5f);
            const float x1 = map.dbToX(t + w * 0.5f);
            g.setColour(palette::col(palette::accent).withAlpha(0.07f));
            g.fillRect(juce::Rectangle<float>(x0, r.getY(), x1 - x0, r.getHeight()));
        }

        // Threshold marker (dynamics -> accent, dimmed so the curve stays focal).
        g.setColour(palette::col(palette::accentDim));
        const float tx = map.dbToX(t);
        g.drawLine(tx, r.getY(), tx, r.getBottom(), 1.0f);

        // The transfer curve itself: the plugin's action -> warm accent.
        g.setColour(palette::col(palette::accent));
        g.strokePath(curvePath, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved));

        g.setColour(palette::col(palette::axis));
        g.drawRoundedRectangle(r.reduced(0.5f), metrics::kCorner, 1.0f);
    }

private:
    void rebuildPath() {
        curvePath.clear();
        if (getWidth() <= 0)
            return;
        const double t = threshold->load();
        const double rv = ratio->load();
        const double w = knee->load();
        const double rg = range->load();

        // ~1 px sampling across the input axis; linear map so this is exact
        // enough everywhere including the knee parabola.
        const int n = juce::jmax(2, getWidth());
        for (int i = 0; i < n; ++i) {
            const double inDb = compui::CompCurveMap::kMinDb +
                                (compui::CompCurveMap::kMaxDb - compui::CompCurveMap::kMinDb) *
                                    static_cast<double>(i) / (n - 1);
            const float x = map.dbToX(static_cast<float>(inDb));
            const float y = map.dbToY(static_cast<float>(curveOutDb(inDb, t, rv, w, rg)));
            if (i == 0)
                curvePath.startNewSubPath(x, y);
            else
                curvePath.lineTo(x, y);
        }
    }

    juce::AudioProcessorValueTreeState& apvts;
    std::atomic<float>* threshold = nullptr;
    std::atomic<float>* ratio = nullptr;
    std::atomic<float>* knee = nullptr;
    std::atomic<float>* range = nullptr;

    compui::CompCurveMap map;
    juce::Path curvePath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransferCurveView)
};

/**
    Live IO dot overlay: sits exactly on top of TransferCurveView (same bounds)
    and repaints at the editor's 30 Hz meter rate — cheap (one circle), so the
    curve view underneath never re-tessellates per tick. The dot marks
    (x_L, curve(x_L)): where the DETECTOR currently sits on the static curve.
    Hidden when the detector level is below the plot window (silence).
*/
class IoDotLayer : public juce::Component {
public:
    explicit IoDotLayer(const TransferCurveView& curveIn) : curve(curveIn) {
        setInterceptsMouseClicks(false, false);
        setOpaque(false);
    }

    /** Editor timer: latest published detector level + current APVTS statics. */
    void update(float scDb, float thresholdDb, float ratioV, float kneeDb, float rangeDb) {
        dotInDb = scDb;
        dotOutDb = static_cast<float>(TransferCurveView::curveOutDb(scDb, thresholdDb, ratioV,
                                                                    kneeDb, rangeDb));
        repaint();
    }

    void paint(juce::Graphics& g) override {
        if (dotInDb <= compui::CompCurveMap::kMinDb)
            return;  // silence: no dot
        const auto& map = curve.getMap();
        const float x = map.dbToX(dotInDb);
        const float y = map.dbToY(dotOutDb);
        g.setColour(lpng::palette::col(lpng::palette::accentBright));
        g.fillEllipse(x - kDotR, y - kDotR, kDotR * 2.0f, kDotR * 2.0f);
    }

private:
    static constexpr float kDotR = 3.5f;

    const TransferCurveView& curve;
    float dotInDb = -120.0f;
    float dotOutDb = -120.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IoDotLayer)
};
