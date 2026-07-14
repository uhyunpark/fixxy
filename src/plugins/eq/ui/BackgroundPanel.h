#pragma once

#include <array>

#include <juce_gui_basics/juce_gui_basics.h>

#include "CoordMap.h"

/**
    Static log-frequency grid + dB rulers behind the response curve. Pure
    paint()-time drawing, no per-frame recomputation — resized() just updates
    the shared CoordMap's bounds.
*/
class BackgroundPanel : public juce::Component {
public:
    explicit BackgroundPanel(CoordMap& mapIn) : map(mapIn) {}

    void paint(juce::Graphics& g) override {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff1a1a1e));
        g.fillRect(bounds);

        // Log-spaced frequency gridlines with labels at standard decades.
        static const std::array<std::pair<float, const char*>, 10> freqLines{{
            {20.f, "20"}, {50.f, "50"}, {100.f, "100"}, {200.f, "200"}, {500.f, "500"},
            {1000.f, "1k"}, {2000.f, "2k"}, {5000.f, "5k"}, {10000.f, "10k"}, {20000.f, "20k"},
        }};

        g.setFont(juce::FontOptions(11.0f));
        for (const auto& [freq, label] : freqLines) {
            const float x = map.freqToX(freq);
            const bool major = (freq == 100.f || freq == 1000.f || freq == 10000.f);
            g.setColour(juce::Colour(major ? 0xff3a3a40 : 0xff28282e));
            g.drawVerticalLine(static_cast<int>(x), bounds.getY(), bounds.getBottom());

            g.setColour(juce::Colour(0xff707078));
            g.drawText(label, static_cast<int>(x) - 15, static_cast<int>(bounds.getBottom()) - 14,
                       30, 14, juce::Justification::centred);
        }

        // dB rulers every 6 dB from -24 to +24, 0 dB emphasized.
        for (int db = -24; db <= 24; db += 6) {
            const float y = map.dbToY(static_cast<float>(db));
            g.setColour(juce::Colour(db == 0 ? 0xff45454e : 0xff28282e));
            g.drawHorizontalLine(static_cast<int>(y), bounds.getX(), bounds.getRight());

            g.setColour(juce::Colour(0xff707078));
            g.drawText(juce::String(db), 2, static_cast<int>(y) - 7, 30, 14,
                       juce::Justification::centredLeft);
        }

        g.setColour(juce::Colour(0xff45454e));
        g.drawRect(bounds, 1.0f);
    }

    void resized() override { map.setBounds(getLocalBounds().toFloat()); }

private:
    CoordMap& map;
};
