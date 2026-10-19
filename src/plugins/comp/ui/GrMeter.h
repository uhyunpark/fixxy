#pragma once

#include <cmath>

#include <juce_gui_basics/juce_gui_basics.h>

#include "CompMeterMap.h"
#include "plugins/FixxyLookAndFeel.h"

/**
    Vertical gain-reduction meter — the family's focal point, so it is the one
    meter drawn in the WARM accent (dynamics action, per the palette rule).

    Scale is the GR meter's OWN: 0..-30 dB, drawn TOP-DOWN (more reduction =
    bar reaches further down), fed with the POSITIVE magnitude the processor
    publishes — a different quantity and a different scale from the dBFS
    IN/OUT meters, mapped by its own GrMeterMap (see CompMeterMap.h).

    Ballistics here are DISPLAY smoothing only (instant attack, ~300 ms
    exponential release at the 30 Hz timer rate, 2 s peak-hold) — they make the
    meter readable, they are NOT the compressor's envelope follower (that lives
    in CompressorNode's detect() and already shaped the published value). The
    numeric readout below the bar deliberately shows the RAW unsmoothed block
    value so the two never get conflated.
*/
class GrMeter : public juce::Component {
public:
    GrMeter() { setInterceptsMouseClicks(false, false); }

    /** Called from the editor's 30 Hz timer with the latest published |GR| dB. */
    void push(float rawGrDb) {
        rawDb = rawGrDb;
        displayDb = rawGrDb >= displayDb ? rawGrDb : rawGrDb + kRelease * (displayDb - rawGrDb);

        if (rawGrDb >= peakDb) {
            peakDb = rawGrDb;
            peakTicksLeft = kPeakHoldTicks;
        } else if (peakTicksLeft > 0) {
            --peakTicksLeft;
        } else {
            peakDb = displayDb;
        }
        repaint();
    }

    void paint(juce::Graphics& g) override {
        using namespace fixxy;
        auto r = getLocalBounds().toFloat();

        auto labelArea = r.removeFromTop(kLabelH);
        g.setColour(palette::col(palette::textSecondary));
        g.setFont(labelFont(metrics::kFontSmall));
        g.drawText("GR", labelArea, juce::Justification::centred);

        // Raw numeric readout below the bar: the UNSMOOTHED block value, in the
        // family's monospaced value font.
        auto readout = r.removeFromBottom(kReadoutH);

        auto well = r.reduced(2.0f, 0.0f);
        g.setColour(palette::col(palette::displayWell));
        g.fillRoundedRectangle(well, metrics::kCorner);

        // Ticks every 6 dB of reduction (chrome).
        g.setColour(palette::col(palette::gridMinor));
        for (float gr = 6.0f; gr < compui::GrMeterMap::kMaxGrDb; gr += 6.0f) {
            const float y = yForGr(well, gr);
            g.drawLine(well.getX() + 1.0f, y, well.getRight() - 1.0f, y, 1.0f);
        }

        // GR fill, TOP-DOWN (warm accent: this is the plugin acting on the signal).
        const float frac = compui::GrMeterMap::fraction(displayDb);
        if (frac > 0.0f) {
            auto fill = well.reduced(2.0f);
            fill = fill.removeFromTop(frac * fill.getHeight());
            g.setColour(palette::col(palette::accent));
            g.fillRect(fill);
        }

        // Peak-hold marker (deepest recent reduction).
        if (peakDb > 0.05f) {
            const float y = yForGr(well.reduced(2.0f), peakDb);
            g.setColour(palette::col(palette::accentBright));
            g.drawLine(well.getX() + 2.0f, y, well.getRight() - 2.0f, y, 1.0f);
        }

        g.setColour(palette::col(palette::axis));
        g.drawRoundedRectangle(well.reduced(0.5f), metrics::kCorner, 1.0f);

        g.setColour(palette::col(palette::textPrimary));
        g.setFont(valueFont(metrics::kFontSmall));
        g.drawText(juce::String(-rawDb, 1), readout, juce::Justification::centred);
    }

    static constexpr float kLabelH = 14.0f;
    static constexpr float kReadoutH = 14.0f;

private:
    static float yForGr(juce::Rectangle<float> bar, float grDb) {
        // Top-down: y grows with reduction from the bar's top edge.
        return bar.getY() + compui::GrMeterMap::fraction(grDb) * bar.getHeight();
    }

    static constexpr float kRelease = 0.89484f;  // exp(-1/(30*0.3)) — display only
    static constexpr int kPeakHoldTicks = 60;    // 2 s at 30 Hz

    float rawDb = 0.0f;      // unsmoothed block value (readout)
    float displayDb = 0.0f;  // ballistic bar value (display only)
    float peakDb = 0.0f;
    int peakTicksLeft = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrMeter)
};
