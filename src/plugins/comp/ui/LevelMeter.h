#pragma once

#include <cmath>

#include <juce_gui_basics/juce_gui_basics.h>

#include "CompMeterMap.h"
#include "plugins/FixxyLookAndFeel.h"

/**
    Vertical IN/OUT peak meter (dBFS, -60..0, bottom-up fill, cool data colour
    — it shows the SIGNAL, so per the family palette rule it never uses the
    warm accent).

    The editor's single 30 Hz timer calls push() with the latest published
    block peak; this component only applies DISPLAY ballistics and repaints.
    Display ballistics are a UI readability device — instant attack so a
    transient registers at full height, exponential release (tau ~300 ms) so
    the eye can follow the fall, 2 s peak-hold marker. They are NOT the
    compressor's envelope follower; the audio-path ballistics live in
    CompressorNode and are untouched by anything here.
*/
class LevelMeter : public juce::Component {
public:
    explicit LevelMeter(juce::String labelIn) : label(std::move(labelIn)) {
        setInterceptsMouseClicks(false, false);
    }

    /** Called from the editor's 30 Hz timer with the latest published dBFS peak. */
    void push(float rawDb) {
        // Instant attack; exponential release toward the new value. kRelease
        // is exp(-1/(30 Hz * 0.3 s)) — a ~300 ms decay tau at the timer rate.
        displayDb = rawDb >= displayDb ? rawDb : rawDb + kRelease * (displayDb - rawDb);

        if (rawDb >= peakDb) {
            peakDb = rawDb;
            peakTicksLeft = kPeakHoldTicks;
        } else if (peakTicksLeft > 0) {
            --peakTicksLeft;
        } else {
            peakDb = displayDb;  // hold expired: marker rejoins the falling bar
        }
        repaint();
    }

    void paint(juce::Graphics& g) override {
        using namespace fixxy;
        auto r = getLocalBounds().toFloat();

        // Label strip on top, bar well below.
        auto labelArea = r.removeFromTop(kLabelH);
        g.setColour(palette::col(palette::textSecondary));
        g.setFont(labelFont(metrics::kFontSmall));
        g.drawText(label, labelArea, juce::Justification::centred);

        auto well = r.reduced(2.0f, 0.0f);
        g.setColour(palette::col(palette::displayWell));
        g.fillRoundedRectangle(well, metrics::kCorner);

        // Ticks every 12 dB (chrome — structure only).
        g.setColour(palette::col(palette::gridMinor));
        for (float db = compui::LevelMeterMap::kMinDb + 12.0f;
             db < compui::LevelMeterMap::kMaxDb; db += 12.0f) {
            const float y = yForDb(well, db);
            g.drawLine(well.getX() + 1.0f, y, well.getRight() - 1.0f, y, 1.0f);
        }

        // Level fill, bottom-up (cool: this is the signal).
        const float frac = compui::LevelMeterMap::fraction(displayDb);
        if (frac > 0.0f) {
            auto fill = well.reduced(2.0f);
            fill = fill.removeFromBottom(frac * fill.getHeight());
            g.setColour(palette::col(palette::dataPost).withAlpha(0.85f));
            g.fillRect(fill);
        }

        // Peak-hold marker.
        if (peakDb > compui::LevelMeterMap::kMinDb) {
            const float y = yForDb(well.reduced(2.0f), peakDb);
            g.setColour(palette::col(palette::dataSum));
            g.drawLine(well.getX() + 2.0f, y, well.getRight() - 2.0f, y, 1.0f);
        }

        g.setColour(palette::col(palette::axis));
        g.drawRoundedRectangle(well.reduced(0.5f), metrics::kCorner, 1.0f);
    }

    static constexpr float kLabelH = 14.0f;

private:
    static float yForDb(juce::Rectangle<float> bar, float db) {
        return bar.getBottom() - compui::LevelMeterMap::fraction(db) * bar.getHeight();
    }

    // Display-ballistics constants (see class comment: UI smoothing only).
    static constexpr float kRelease = 0.89484f;  // exp(-1/(30*0.3))
    static constexpr int kPeakHoldTicks = 60;    // 2 s at 30 Hz

    juce::String label;
    float displayDb = compui::LevelMeterMap::kMinDb;
    float peakDb = compui::LevelMeterMap::kMinDb;
    int peakTicksLeft = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LevelMeter)
};
