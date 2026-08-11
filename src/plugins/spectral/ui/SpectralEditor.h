#pragma once

#include <array>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "plugins/LpngLookAndFeel.h"
#include "plugins/spectral/PluginProcessor.h"
#include "plugins/spectral/ui/SpectralAnalyzerView.h"

/**
    LpngSpectral custom editor — the debut of the family's LIGHT visual language
    (docs/design-reference.md, mockup variant B "Family Light"). Layout
    (resizable, default 1000x640):

      header  : "LpngSpectral" strip (cream, hairline under)
      body    : LEFT RAIL (stone) — hero DEPTH knob (ember = the action knob),
                detail knob (azure), attack + release knobs; each with a value
                chip. MAIN DISPLAY (white well) — SpectralAnalyzerView: live input
                spectrum (azure) + ember reduction ribbon.
      bottom  : bypass + DELTA pills (delta = ember when active), mix % chip,
                out dB chip.

    Palette rule (LpngLookAndFeel light spec): DEPTH is the amount the processor
    acts by -> warm ember fill; detail/attack/release are cool azure; the ribbon
    and the active delta pill carry the ember action accent; everything else is
    monochrome paper. All chrome/data colour comes from lpng::palettelight — no
    hardcoded hex here.

    No side-tab strip, no soft/hard toggle, no quality dropdown in v1 (those
    params do not exist yet) — deferred, see the task report.
*/
class SpectralEditor : public juce::AudioProcessorEditor {
public:
    explicit SpectralEditor(SpectralProcessor& p)
        : AudioProcessorEditor(p), proc(p),
          display(p.specFifo, p.reduction, p.publishedSampleRate) {
        setLookAndFeel(&lnf);

        addAndMakeVisible(display);

        for (std::size_t i = 0; i < kNumKnobs; ++i) {
            auto& k = knobs[i];
            addAndMakeVisible(k.name);
            k.name.setText(kKnobDefs[i].title, juce::dontSendNotification);
            k.name.setJustificationType(juce::Justification::centred);
            k.name.setFont(lpng::labelFont(kKnobDefs[i].hero ? lpng::metrics::kFontHeader
                                                             : lpng::metrics::kFontLabel));
            k.name.setColour(juce::Label::textColourId,
                             lpng::palettelight::col(kKnobDefs[i].hero
                                                         ? lpng::palettelight::textPrimary
                                                         : lpng::palettelight::textSecondary));

            addAndMakeVisible(k.slider);
            k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 18);
            k.slider.setColour(juce::Slider::rotarySliderFillColourId,
                               lpng::palettelight::col(kKnobDefs[i].action
                                                           ? lpng::palettelight::accent
                                                           : lpng::palettelight::controlFill));
            k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                proc.apvts, kKnobDefs[i].paramId, k.slider);
        }

        // Bottom bar: two pills + two chip-sliders.
        addAndMakeVisible(bypassButton);
        bypassButton.setButtonText("bypass");
        bypassButton.setColour(juce::ToggleButton::tickColourId,
                               lpng::palettelight::col(lpng::palettelight::pillOnBg));  // ink
        bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            proc.apvts, "bypass", bypassButton);

        addAndMakeVisible(deltaButton);
        deltaButton.setButtonText("delta");
        deltaButton.setColour(juce::ToggleButton::tickColourId,
                              lpng::palettelight::col(lpng::palettelight::accent));  // ember
        deltaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            proc.apvts, "delta", deltaButton);

        for (std::size_t i = 0; i < kNumChips; ++i) {
            auto& c = chips[i];
            addAndMakeVisible(c.name);
            c.name.setText(kChipDefs[i].title, juce::dontSendNotification);
            c.name.setJustificationType(juce::Justification::centredRight);
            c.name.setFont(lpng::labelFont());
            c.name.setColour(juce::Label::textColourId,
                             lpng::palettelight::col(lpng::palettelight::textSecondary));

            addAndMakeVisible(c.slider);
            c.slider.setSliderStyle(juce::Slider::LinearBar);
            c.slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            c.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                proc.apvts, kChipDefs[i].paramId, c.slider);
        }

        setResizable(true, true);
        setResizeLimits(760, 520, 2000, 1400);
        setSize(1000, 640);
    }

    ~SpectralEditor() override { setLookAndFeel(nullptr); }

    void paint(juce::Graphics& g) override {
        namespace pl = lpng::palettelight;
        g.fillAll(pl::col(pl::window));

        auto r = getLocalBounds();
        auto header = r.removeFromTop(kHeaderH);
        auto bottom = r.removeFromBottom(kBottomH);
        auto rail = r.removeFromLeft(kRailW);

        // Left rail: stone surface with a hairline separation.
        g.setColour(pl::col(pl::panel));
        g.fillRect(rail);
        g.setColour(pl::col(pl::hairline));
        g.drawVerticalLine(rail.getRight() - 1, static_cast<float>(rail.getY()),
                           static_cast<float>(rail.getBottom()));

        // Header + bottom bar: cream with hairline dividers (no shadow).
        g.setColour(pl::col(pl::hairline));
        g.drawHorizontalLine(header.getBottom() - 1, 0.0f, static_cast<float>(getWidth()));
        g.drawHorizontalLine(bottom.getY(), 0.0f, static_cast<float>(getWidth()));

        g.setColour(pl::col(pl::textPrimary));
        g.setFont(lpng::headerFont(16.0f));
        g.drawText("LpngSpectral", header.reduced(2 * lpng::metrics::kSpacing, 0),
                   juce::Justification::centredLeft);
        g.setColour(pl::col(pl::textMuted));
        g.setFont(lpng::labelFont(lpng::metrics::kFontSmall));
        g.drawText("SPECTRAL", header.reduced(2 * lpng::metrics::kSpacing, 0),
                   juce::Justification::centredRight);
    }

    void resized() override {
        using namespace lpng::metrics;
        auto r = getLocalBounds();
        r.removeFromTop(kHeaderH);
        auto bottom = r.removeFromBottom(kBottomH);
        auto rail = r.removeFromLeft(kRailW);

        display.setBounds(r.reduced(kSpacing));

        // --- left rail: hero depth, detail, then attack|release row ---
        auto rc = rail.reduced(18, 16);
        layoutKnob(knobs[0], rc.removeFromTop(kKnobL + 40), kKnobL);   // DEPTH (hero)
        rc.removeFromTop(14);
        layoutKnob(knobs[1], rc.removeFromTop(kKnobM + 38), kKnobM);   // detail
        rc.removeFromTop(14);
        auto smallRow = rc.removeFromTop(kKnobS + 38);
        layoutKnob(knobs[2], smallRow.removeFromLeft(smallRow.getWidth() / 2), kKnobS);  // attack
        layoutKnob(knobs[3], smallRow, kKnobS);                                          // release

        // --- bottom bar: pills left, chips right ---
        auto b = bottom.reduced(2 * kSpacing, 0);
        const int pillH = 26;
        auto pillRow = b.removeFromLeft(160).withSizeKeepingCentre(160, pillH);
        bypassButton.setBounds(pillRow.removeFromLeft(74));
        pillRow.removeFromLeft(8);
        deltaButton.setBounds(pillRow.removeFromLeft(74));

        // chips laid out from the right: out then mix.
        layoutChip(chips[1], b.removeFromRight(84), pillH);  // out
        b.removeFromRight(kSpacing);
        layoutChip(chips[0], b.removeFromRight(72), pillH);  // mix
    }

private:
    static constexpr int kHeaderH = 44;
    static constexpr int kBottomH = 46;
    static constexpr int kRailW = 208;

    struct KnobDef {
        const char* paramId;
        const char* title;
        bool action;  // true -> ember fill (the amount the processor acts by)
        bool hero;    // true -> larger, ink uppercase-ish name
    };
    static constexpr std::size_t kNumKnobs = 4;
    static inline const std::array<KnobDef, kNumKnobs> kKnobDefs{{
        {"depth", "DEPTH", true, true},
        {"detail", "detail", false, false},
        {"attack", "attack", false, false},
        {"release", "release", false, false},
    }};

    struct ChipDef { const char* paramId; const char* title; };
    static constexpr std::size_t kNumChips = 2;
    static inline const std::array<ChipDef, kNumChips> kChipDefs{{
        {"mix", "mix"},
        {"output", "out"},
    }};

    struct Knob {
        juce::Label name;
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    struct Chip {
        juce::Label name;
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void layoutKnob(Knob& k, juce::Rectangle<int> cell, int knobPx) {
        k.name.setBounds(cell.removeFromTop(18));
        const int w = juce::jmin(cell.getWidth(), juce::jmax(knobPx, 60));
        const int h = juce::jmin(cell.getHeight(), knobPx + 20);
        k.slider.setBounds(cell.withSizeKeepingCentre(w, h));
    }

    void layoutChip(Chip& c, juce::Rectangle<int> cell, int chipH) {
        auto row = cell.withSizeKeepingCentre(cell.getWidth(), chipH);
        c.name.setBounds(row.removeFromLeft(34));
        c.slider.setBounds(row);
    }

    SpectralProcessor& proc;
    lpng::LpngLookAndFeelLight lnf;  // must outlive the children below

    SpectralAnalyzerView display;

    std::array<Knob, kNumKnobs> knobs;
    std::array<Chip, kNumChips> chips;

    juce::ToggleButton bypassButton, deltaButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> deltaAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralEditor)
};
