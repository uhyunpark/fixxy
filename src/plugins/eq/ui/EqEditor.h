#pragma once

#include <array>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "AnalyzerView.h"
#include "BackgroundPanel.h"
#include "CoordMap.h"
#include "CurveView.h"
#include "DraggerLayer.h"
#include "plugins/eq/PluginProcessor.h"

/**
    Stage 1-3 custom editor: response-curve view (BackgroundPanel + CurveView
    + AnalyzerView, sharing one CoordMap) topped by DraggerLayer for
    click/drag/wheel/keyboard band editing, with a functional band-control
    strip underneath.

    The strip's band-select combo and DraggerLayer's click-to-select are kept
    in sync (see rebuildAttachments()/onSelectionChanged below): selecting a
    band either way updates both the strip's attachments and the emphasized
    curve/handle.
*/
class EqEditor : public juce::AudioProcessorEditor {
public:
    explicit EqEditor(MetaPluginProcessor& p)
        : AudioProcessorEditor(p), proc(p), background(map),
          analyzer(p.preFifo, p.postFifo, map, p.publishedSampleRate),
          curve(p.apvts, map, p.publishedSampleRate),
          dragger(p.apvts, map, curve, p.publishedSampleRate) {
        addAndMakeVisible(background);
        addAndMakeVisible(analyzer);  // behind the curve, above the grid
        addAndMakeVisible(curve);
        addAndMakeVisible(dragger);  // topmost: intercepts mouse for drag interaction

        addAndMakeVisible(bandSelect);
        for (int i = 0; i < MetaPluginProcessor::kNumBands; ++i)
            bandSelect.addItem("Band " + juce::String(i + 1), i + 1);
        bandSelect.onChange = [this] {
            rebuildAttachments();
            const int band = bandSelect.getSelectedId() - 1;
            dragger.setSelectedBandExternal(band);
        };

        // DraggerLayer selection (click/double-click on the curve) drives the
        // strip the other way. Only fires on an actual selection change (see
        // DraggerLayer::selectBand), and never on setSelectedBandExternal, so
        // this can't loop back into bandSelect.onChange above. -1 (deselect,
        // e.g. clicking empty area) intentionally leaves the strip on the
        // last band -- the strip has no "none" state to fall back to.
        dragger.onSelectionChanged = [this](int band) {
            if (band < 0) return;
            bandSelect.setSelectedId(band + 1, juce::dontSendNotification);
            rebuildAttachments();
        };

        addAndMakeVisible(enabledLabel);
        addAndMakeVisible(enabledButton);

        addAndMakeVisible(typeLabel);
        addAndMakeVisible(typeBox);
        for (auto& choice : kTypeChoices) typeBox.addItem(choice, typeBox.getNumItems() + 1);

        addAndMakeVisible(freqLabel);
        addAndMakeVisible(freqSlider);
        freqSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        freqSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 16);

        addAndMakeVisible(qLabel);
        addAndMakeVisible(qSlider);
        qSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        qSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 16);

        addAndMakeVisible(gainLabel);
        addAndMakeVisible(gainSlider);
        gainSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        gainSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 16);

        addAndMakeVisible(slopeLabel);
        addAndMakeVisible(slopeBox);
        for (auto& choice : kSlopeChoices) slopeBox.addItem(choice, slopeBox.getNumItems() + 1);

        bandSelect.setSelectedId(1, juce::dontSendNotification);
        rebuildAttachments();
        dragger.setSelectedBandExternal(0);

        setResizable(true, true);
        setResizeLimits(600, 420, 2400, 1600);
        setSize(900, 600);
    }

    void resized() override {
        auto r = getLocalBounds();
        auto controls = r.removeFromBottom(96);
        background.setBounds(r);
        analyzer.setBounds(r);
        curve.setBounds(r);
        dragger.setBounds(r);

        controls.reduce(6, 6);
        auto bandArea = controls.removeFromLeft(90);
        bandSelect.setBounds(bandArea.removeFromTop(24));

        const int n = 6;
        const int w = controls.getWidth() / n;
        auto layoutKnob = [&](juce::Label& label, juce::Component& ctrl) {
            auto area = controls.removeFromLeft(w);
            label.setBounds(area.removeFromTop(18));
            ctrl.setBounds(area);
        };
        layoutKnob(enabledLabel, enabledButton);
        layoutKnob(typeLabel, typeBox);
        layoutKnob(freqLabel, freqSlider);
        layoutKnob(qLabel, qSlider);
        layoutKnob(gainLabel, gainSlider);
        layoutKnob(slopeLabel, slopeBox);
    }

private:
    static juce::String bandID(int i, const char* suffix) {
        return "band" + juce::String(i + 1) + suffix;
    }

    void rebuildAttachments() {
        const int band = bandSelect.getSelectedId() - 1;
        if (band < 0) return;

        using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

        enabledAttachment.reset();
        typeAttachment.reset();
        freqAttachment.reset();
        qAttachment.reset();
        gainAttachment.reset();
        slopeAttachment.reset();

        enabledAttachment =
            std::make_unique<ButtonAttachment>(proc.apvts, bandID(band, "Enabled"), enabledButton);
        typeAttachment = std::make_unique<ComboAttachment>(proc.apvts, bandID(band, "Type"), typeBox);
        freqAttachment = std::make_unique<SliderAttachment>(proc.apvts, bandID(band, "Freq"), freqSlider);
        qAttachment = std::make_unique<SliderAttachment>(proc.apvts, bandID(band, "Q"), qSlider);
        gainAttachment = std::make_unique<SliderAttachment>(proc.apvts, bandID(band, "Gain"), gainSlider);
        slopeAttachment = std::make_unique<ComboAttachment>(proc.apvts, bandID(band, "Slope"), slopeBox);
    }

    // Must match the choice order in PluginProcessor.cpp exactly.
    static inline const juce::StringArray kTypeChoices{
        "Bell", "Low Shelf", "High Shelf", "Tilt", "Notch", "Bandpass", "Lowpass", "Highpass"};
    static inline const juce::StringArray kSlopeChoices{
        "6", "12", "18", "24", "30", "36", "48", "72", "96"};

    MetaPluginProcessor& proc;

    CoordMap map;
    BackgroundPanel background;
    AnalyzerView analyzer;
    CurveView curve;
    DraggerLayer dragger;

    juce::ComboBox bandSelect;

    juce::Label enabledLabel{{}, "On"};
    juce::ToggleButton enabledButton;

    juce::Label typeLabel{{}, "Type"};
    juce::ComboBox typeBox;

    juce::Label freqLabel{{}, "Freq"};
    juce::Slider freqSlider;

    juce::Label qLabel{{}, "Q"};
    juce::Slider qSlider;

    juce::Label gainLabel{{}, "Gain"};
    juce::Slider gainSlider;

    juce::Label slopeLabel{{}, "Slope"};
    juce::ComboBox slopeBox;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> freqAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> qAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> slopeAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqEditor)
};
