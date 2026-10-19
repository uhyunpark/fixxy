#pragma once

#include <array>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "GrMeter.h"
#include "LevelMeter.h"
#include "TransferCurveView.h"
#include "plugins/LpngLookAndFeel.h"
#include "plugins/comp/PluginProcessor.h"

/**
    LpngComp v2 editor. Layout (resizable, default 900x600):

      header   : product name strip
      display  : transfer curve (left ~75%) + fixed-width meter zone
                 (IN | GR | OUT vertical meters) on the right
      knobs    : Threshold(L) Ratio(L) Knee(M) | Attack(M) Release(M) Hold(S)
                 | Makeup(M) Mix(M) Range(M)   — grouped by function
      toggles  : AutoMakeup, ProgRel, SC HPF, RMS

    Palette rule (LpngLookAndFeel spec): threshold/ratio/range knobs get the
    warm accent fill (dynamics action); every other knob keeps the cool
    controlFill; meters showing LEVEL are cool, the GR meter and the transfer
    curve (the plugin acting on the signal) carry the accent.

    Threading: one 30 Hz timer polls the processor's published atomics and
    pushes them into the meters/dot (message thread only). APVTS listener
    callbacks may arrive on any thread (host automation), so parameterChanged()
    only flags an AsyncUpdater; handleAsyncUpdate() does the actual curve
    rebuild — the EqEditor pattern. The curve path is rebuilt ONLY on parameter
    change, never per tick.
*/
class CompEditor : public juce::AudioProcessorEditor,
                   private juce::AudioProcessorValueTreeState::Listener,
                   private juce::AsyncUpdater,
                   private juce::Timer {
public:
    explicit CompEditor(CompProcessor& p) : AudioProcessorEditor(p), proc(p), curve(p.apvts), dot(curve) {
        setLookAndFeel(&lnf);

        addAndMakeVisible(curve);
        addAndMakeVisible(dot);
        addAndMakeVisible(inMeter);
        addAndMakeVisible(grMeter);
        addAndMakeVisible(outMeter);

        for (std::size_t i = 0; i < kNumKnobs; ++i) {
            auto& k = knobs[i];
            addAndMakeVisible(k.label);
            k.label.setText(kKnobDefs[i].title, juce::dontSendNotification);
            k.label.setJustificationType(juce::Justification::centred);
            k.label.setFont(lpng::labelFont());

            addAndMakeVisible(k.slider);
            k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 15);
            if (kKnobDefs[i].dynamics)  // dynamics knob agrees with the GR meter
                k.slider.setColour(juce::Slider::rotarySliderFillColourId,
                                   lpng::palette::col(lpng::palette::accent));
            k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                proc.apvts, kKnobDefs[i].paramId, k.slider);
        }

        for (std::size_t i = 0; i < kNumToggles; ++i) {
            auto& t = toggles[i];
            addAndMakeVisible(t.button);
            t.button.setButtonText(kToggleDefs[i].title);
            t.attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
                proc.apvts, kToggleDefs[i].paramId, t.button);
        }

        // Curve statics: rebuild only when these change (any-thread callback ->
        // async flag; see class comment). Range is included because the drawn
        // curve folds the range clamp in when active.
        for (auto* id : kCurveParams)
            proc.apvts.addParameterListener(id, this);

        startTimerHz(30);

        setResizable(true, true);
        setResizeLimits(720, 480, 2400, 1600);
        setSize(900, 600);
    }

    ~CompEditor() override {
        for (auto* id : kCurveParams)
            proc.apvts.removeParameterListener(id, this);
        cancelPendingUpdate();
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override {
        using namespace lpng;
        g.fillAll(palette::col(palette::window));

        // Header strip: product name (chrome only).
        auto header = getLocalBounds().removeFromTop(kHeaderH);
        g.setColour(palette::col(palette::panel));
        g.fillRect(header);
        g.setColour(palette::col(palette::textPrimary));
        g.setFont(headerFont());
        g.drawText(JucePlugin_Name, header.reduced(2 * metrics::kSpacing, 0),
                   juce::Justification::centredLeft);
        g.setColour(palette::col(palette::textMuted));
        g.setFont(labelFont(metrics::kFontSmall));
        g.drawText("COMPRESSOR", header.reduced(2 * metrics::kSpacing, 0),
                   juce::Justification::centredRight);
    }

    void resized() override {
        using namespace lpng::metrics;
        auto r = getLocalBounds();
        r.removeFromTop(kHeaderH);

        // --- bottom control zone: toggle row + knob strip ---
        auto toggleRow = r.removeFromBottom(kToggleRowH).reduced(2 * kSpacing, kSpacing / 2);
        const int toggleW = juce::jmin(140, toggleRow.getWidth() / static_cast<int>(kNumToggles));
        for (auto& t : toggles)
            t.button.setBounds(toggleRow.removeFromLeft(toggleW));

        auto strip = r.removeFromBottom(kKnobStripH).reduced(kSpacing, kSpacing);
        const int groupGap = 2 * kSpacing;
        const int colW = (strip.getWidth() - 2 * groupGap) / static_cast<int>(kNumKnobs);
        for (std::size_t i = 0; i < kNumKnobs; ++i) {
            if (i == 3 || i == 6)
                strip.removeFromLeft(groupGap);  // gap between functional groups
            auto cell = strip.removeFromLeft(colW);
            knobs[i].label.setBounds(cell.removeFromTop(14));
            // Constrain the rotary to its importance size (S/M/L) + textbox.
            const int knobPx = kKnobDefs[i].size;
            const int wantW = juce::jmin(cell.getWidth(), juce::jmax(knobPx, 64));
            const int wantH = juce::jmin(cell.getHeight(), knobPx + 17);
            knobs[i].slider.setBounds(cell.withSizeKeepingCentre(wantW, wantH));
        }

        // --- display zone: curve left, meters right ---
        auto display = r.reduced(kSpacing);
        auto meterZone = display.removeFromRight(kMeterZoneW);
        display.removeFromRight(kSpacing);
        curve.setBounds(display);
        dot.setBounds(display);

        // Reserve the readout strip in all three columns so the bars align;
        // only GR draws text in it.
        const int meterW = meterZone.getWidth() / 3;
        auto inA = meterZone.removeFromLeft(meterW);
        auto grA = meterZone.removeFromLeft(meterW);
        auto outA = meterZone;
        inA.removeFromBottom(static_cast<int>(GrMeter::kReadoutH));
        outA.removeFromBottom(static_cast<int>(GrMeter::kReadoutH));
        inMeter.setBounds(inA);
        grMeter.setBounds(grA);
        outMeter.setBounds(outA);
    }

private:
    static constexpr int kHeaderH = 28;
    static constexpr int kKnobStripH = 118;
    static constexpr int kToggleRowH = 26;
    static constexpr int kMeterZoneW = 90;

    struct KnobDef {
        const char* paramId;
        const char* title;
        int size;       // lpng::metrics::kKnobS/M/L by importance
        bool dynamics;  // true -> warm accent fill (palette rule)
    };
    static constexpr std::size_t kNumKnobs = 9;
    static inline const std::array<KnobDef, kNumKnobs> kKnobDefs{{
        {"threshold", "Threshold", lpng::metrics::kKnobL, true},
        {"ratio", "Ratio", lpng::metrics::kKnobL, true},
        {"knee", "Knee", lpng::metrics::kKnobM, false},
        {"attack", "Attack", lpng::metrics::kKnobM, false},
        {"release", "Release", lpng::metrics::kKnobM, false},
        {"hold", "Hold", lpng::metrics::kKnobS, false},
        {"makeup", "Makeup", lpng::metrics::kKnobM, false},
        {"mix", "Mix", lpng::metrics::kKnobM, false},
        {"range", "Range", lpng::metrics::kKnobM, true},
    }};

    struct ToggleDef {
        const char* paramId;
        const char* title;
    };
    static constexpr std::size_t kNumToggles = 4;
    static inline const std::array<ToggleDef, kNumToggles> kToggleDefs{{
        {"autoMakeup", "Auto Makeup"},
        {"progRelease", "Prog Release"},
        {"scHpf", "SC HPF"},
        {"rmsMode", "RMS"},
    }};

    static inline const std::array<const char*, 4> kCurveParams{
        "threshold", "ratio", "knee", "range"};

    // APVTS listener contract: may fire on a non-message thread (automation) —
    // only flag async work here (EqEditor pattern).
    void parameterChanged(const juce::String&, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { curve.paramsChanged(); }

    // 30 Hz meter/dot poll: relaxed atomic loads published by processBlock.
    void timerCallback() override {
        inMeter.push(proc.getInputPeakDb());
        outMeter.push(proc.getOutputPeakDb());
        grMeter.push(proc.getGainReductionDb());
        dot.update(proc.getScLevelDb(),
                   proc.apvts.getRawParameterValue("threshold")->load(),
                   proc.apvts.getRawParameterValue("ratio")->load(),
                   proc.apvts.getRawParameterValue("knee")->load(),
                   proc.apvts.getRawParameterValue("range")->load());
    }

    CompProcessor& proc;

    lpng::LpngLookAndFeel lnf;  // must outlive the children below

    TransferCurveView curve;
    IoDotLayer dot;
    LevelMeter inMeter{"IN"};
    GrMeter grMeter;
    LevelMeter outMeter{"OUT"};

    struct Knob {
        juce::Label label;
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    std::array<Knob, kNumKnobs> knobs;

    struct Toggle {
        juce::ToggleButton button;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    };
    std::array<Toggle, kNumToggles> toggles;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CompEditor)
};
