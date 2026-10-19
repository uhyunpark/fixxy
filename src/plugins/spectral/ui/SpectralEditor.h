#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "dsp/BandResponse.h"
#include "plugins/FixxyLookAndFeel.h"
#include "plugins/spectral/PluginProcessor.h"
#include "plugins/spectral/SpectralStateFormat.h"
#include "plugins/spectral/ui/SpectralAnalyzerView.h"
#include "plugins/spectral/ui/WeightBandLayer.h"

/**
    fixxy Spectral custom editor — the debut of the family's LIGHT visual language
    (docs/design-reference.md, mockup variant B "Family Light"). Layout
    (resizable, default 1132x640):

      header  : "fixxy Spectral" strip (cream, hairline under)
      body    : LEFT RAIL (stone) — hero DEPTH knob (ember = the action knob),
                detail knob (azure), attack + release knobs; each with a value
                chip. SIDEBAR (stone, soothe3-style collapsible column) — msMode
                [L/R|M/S] pill + link/focus chips ("stereo"), tilt detail/attack/
                release chips ("tilt"), max cut/wet trim chips ("scale"). MAIN
                DISPLAY (white well) — SpectralAnalyzerView (live input spectrum
                + ember reduction ribbon) topped by WeightBandLayer (the
                soothe3-style weight-curve band editor: ink composite curve,
                per-band dot handles, range pins) and, when a band is selected,
                a band-edit strip pinned to the bottom of the well.
      bottom  : bypass + DELTA pills (delta = ember when active), mix % chip,
                out dB chip.

    Palette rule (FixxyLookAndFeel light spec): DEPTH is the amount the processor
    acts by -> warm ember fill; detail/attack/release are cool azure; the ribbon
    and the active delta pill carry the ember action accent; the weight curve is
    ink (docs/issues.md DESIGN-DIRECTION), weight-band dots are per-band hues
    (palettelight::bandColour); everything else is monochrome paper. All chrome/
    data colour comes from fixxy::palettelight — no hardcoded hex here.

    Soft/Hard selects the detector; quality and phase controls are not exposed.
*/
class SpectralEditor : public juce::AudioProcessorEditor,
                        private juce::AudioProcessorValueTreeState::Listener,
                        private juce::AsyncUpdater,
                        private juce::Timer {
public:
    explicit SpectralEditor(SpectralProcessor& p)
        : AudioProcessorEditor(p), proc(p),
          display(p.specFifo, p.reduction, p.publishedSampleRate, *p.apvts.getRawParameterValue("delta"),
                  *p.apvts.getRawParameterValue("bypass"),
                  p.cbLastPct, p.cbMaxPctSinceOpen),  // FIELD DIAGNOSTIC (temporary)
          weightLayer(p.apvts, p.publishedSampleRate, p.workflow) {
        // FIELD DIAGNOSTIC (temporary): zero the "max since open" peak so the
        // readout measures from when this editor opened. REMOVE with the readout.
        proc.cbMaxPctSinceOpen.store(0.0f, std::memory_order_relaxed);

        setLookAndFeel(&lnf);

        addAndMakeVisible(display);
        addAndMakeVisible(weightLayer);  // topmost over the well: intercepts mouse for band editing
        weightLayer.onSelectionChanged = [this](int band) {
            if (hasActiveBandSliderGesture()) {
                pendingBandSelection = band;
                return;
            }
            pendingBandSelection.reset();
            selectedBand = band;
            rebuildBandStrip();  // sets each strip component's visibility (own repaint)
            repaint();           // redraw this editor's own strip chrome (panel + badge)
        };

        for (std::size_t i = 0; i < kNumKnobs; ++i) {
            auto& k = knobs[i];
            addAndMakeVisible(k.name);
            k.name.setText(kKnobDefs[i].title, juce::dontSendNotification);
            k.name.setJustificationType(juce::Justification::centred);
            k.name.setFont(fixxy::labelFont(kKnobDefs[i].hero ? fixxy::metrics::kFontHeader
                                                             : fixxy::metrics::kFontLabel));
            k.name.setColour(juce::Label::textColourId,
                             fixxy::palettelight::col(kKnobDefs[i].hero
                                                         ? fixxy::palettelight::textPrimary
                                                         : fixxy::palettelight::textSecondary));

            addAndMakeVisible(k.slider);
            k.slider.setComponentID(kKnobDefs[i].paramId);
            k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 18);
            k.slider.setColour(juce::Slider::rotarySliderFillColourId,
                               fixxy::palettelight::col(kKnobDefs[i].action
                                                           ? fixxy::palettelight::accent
                                                           : fixxy::palettelight::controlFill));
            k.slider.setTooltip(kKnobDefs[i].tooltip);
            k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                proc.apvts, kKnobDefs[i].paramId, k.slider);
            bindSliderGestures(k.slider, kKnobDefs[i].paramId);
        }

        configureWorkflowToolbar();
        for (auto* button : {&segSoft, &segHard}) {
            addAndMakeVisible(*button);
            button->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
            button->setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
        }
        segSoft.setButtonText("Soft");
        segHard.setButtonText("Hard");
        segSoft.setTooltip("Detect resonances relative to their local spectral background.");
        segHard.setTooltip("Detect absolute spectral level; Depth lowers the threshold.");
        segSoft.onClick = [this] { setBoolGestured("detectionMode", false); updateDetectionStyling(); };
        segHard.onClick = [this] { setBoolGestured("detectionMode", true); updateDetectionStyling(); };

        // Bottom bar: two pills + two chip-sliders.
        addAndMakeVisible(bypassButton);
        bypassButton.setButtonText("bypass");
        bypassButton.setTooltip("Listen to the latency-aligned original signal.");
        bypassButton.setColour(juce::ToggleButton::tickColourId,
                               fixxy::palettelight::col(fixxy::palettelight::pillOnBg));  // ink
        bypassButton.onClick = [this] { setBoolGestured("bypass", bypassButton.getToggleState()); };

        addAndMakeVisible(deltaButton);
        deltaButton.setButtonText("delta");
        deltaButton.setTooltip("Listen to the component removed by the processor.");
        deltaButton.setColour(juce::ToggleButton::tickColourId,
                              fixxy::palettelight::col(fixxy::palettelight::accent));  // ember
        deltaButton.onClick = [this] { setBoolGestured("delta", deltaButton.getToggleState()); };

        for (std::size_t i = 0; i < kNumChips; ++i) {
            auto& c = chips[i];
            addAndMakeVisible(c.name);
            c.name.setText(kChipDefs[i].title, juce::dontSendNotification);
            c.name.setJustificationType(juce::Justification::centredRight);
            c.name.setFont(fixxy::labelFont());
            c.name.setColour(juce::Label::textColourId,
                             fixxy::palettelight::col(fixxy::palettelight::textSecondary));

            addAndMakeVisible(c.slider);
            c.slider.setSliderStyle(juce::Slider::LinearBar);
            c.slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            c.slider.setTooltip(i == 0 ? "Blend the original and processed signals."
                                      : "Set the final output level in dB.");
            c.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                proc.apvts, kChipDefs[i].paramId, c.slider);
            bindSliderGestures(c.slider, kChipDefs[i].paramId);
        }

        // Sidebar column: soothe3-style [L/R|M/S] pill. Two TextButtons with
        // clicking-toggles-state off and transparent backgrounds so they only
        // render their text label; the pill track/fill underneath is chrome
        // painted by this editor's own paint() (see the msMode pill block
        // there), same "chrome in paint(), controls as real components" split
        // as the band-edit strip.
        addAndMakeVisible(segLR);
        segLR.setButtonText("L/R");
        segLR.setTooltip("Process the left and right channels.");
        segLR.setClickingTogglesState(false);
        segLR.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        segLR.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
        segLR.onClick = [this] { setBoolGestured("msMode", false); updateMsModeStyling(); };

        addAndMakeVisible(segMS);
        segMS.setButtonText("M/S");
        segMS.setTooltip("Process the mono centre and stereo sides separately.");
        segMS.setClickingTogglesState(false);
        segMS.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        segMS.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
        segMS.onClick = [this] { setBoolGestured("msMode", true); updateMsModeStyling(); };

        updateMsModeStyling();  // establish initial pill styling before first paint

        for (std::size_t i = 0; i < kNumSidebarChips; ++i) {
            auto& c = sidebarChips[i];
            addAndMakeVisible(c.name);
            c.name.setText(kSidebarChipDefs[i].title, juce::dontSendNotification);
            c.name.setJustificationType(juce::Justification::centredRight);
            c.name.setFont(fixxy::labelFont());
            c.name.setColour(juce::Label::textColourId,
                             fixxy::palettelight::col(fixxy::palettelight::textSecondary));

            addAndMakeVisible(c.slider);
            c.slider.setSliderStyle(juce::Slider::LinearBar);
            c.slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            if (kSidebarChipDefs[i].bipolar) c.slider.setDoubleClickReturnValue(true, 0.0);
            c.slider.setComponentID(kSidebarChipDefs[i].paramId);
            c.slider.setTooltip(kSidebarChipDefs[i].tooltip);
            c.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                proc.apvts, kSidebarChipDefs[i].paramId, c.slider);
            bindSliderGestures(c.slider, kSidebarChipDefs[i].paramId);
            if (kSidebarChipDefs[i].bipolar)
                c.slider.textFromValueFunction = [](double value) {
                    return juce::String(std::abs(value) < 0.005 ? 0.0 : value, 2);
                };
        }

        // Band-edit strip: hidden until WeightBandLayer reports a selection
        // (see rebuildBandStrip()). Added AFTER weightLayer so it draws/takes
        // mouse on top of it when visible.
        addChildComponent(bandStripPanel);
        addChildComponent(bandOnButton);
        bandOnButton.setButtonText("enabled");
        bandOnButton.setTooltip("Enable or bypass this weight band without deleting it.");
        bandOnButton.onClick = [this] {
            if (selectedBand >= 0) setBoolGestured(wbandID(selectedBand, "On"), bandOnButton.getToggleState());
        };

        addChildComponent(bandTypeBox);
        bandTypeBox.setTooltip("Choose the shape of this processing-weight band.");
        for (auto& choice : kWeightTypeChoices) bandTypeBox.addItem(choice, bandTypeBox.getNumItems() + 1);
        bandTypeBox.onChange = [this] {
            if (selectedBand >= 0)
                setParameterGestured(wbandID(selectedBand, "Type"), static_cast<float>(bandTypeBox.getSelectedId() - 1));
        };

        for (auto* chip : {&bandFreqChip, &bandGainChip, &bandQChip}) {
            addChildComponent(chip->name);
            chip->name.setJustificationType(juce::Justification::centredRight);
            chip->name.setFont(fixxy::labelFont());
            chip->name.setColour(juce::Label::textColourId,
                                 fixxy::palettelight::col(fixxy::palettelight::textSecondary));
            addChildComponent(chip->slider);
            chip->slider.setSliderStyle(juce::Slider::LinearBar);
            chip->slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        }
        bandFreqChip.name.setText("freq", juce::dontSendNotification);
        bandGainChip.name.setText("depth", juce::dontSendNotification);
        bandQChip.name.setText("q", juce::dontSendNotification);
        bandFreqChip.slider.setTooltip("Set the band's centre or cutoff frequency in Hz.");
        bandGainChip.slider.setTooltip("Raise to suppress more in this region; lower to suppress less. This is not EQ gain.");
        bandQChip.slider.setTooltip("Adjust the band's width or resonance. Scroll over its dot to adjust Q.");

        addChildComponent(bandDeleteButton);
        bandDeleteButton.setButtonText("delete");
        bandDeleteButton.setTooltip("Delete this band and make its slot available for reuse.");
        bandDeleteButton.onClick = [this] {
            if (selectedBand < 0) return;
            const int band = selectedBand;
            proc.workflow.deleteBand(band);
            weightLayer.clearSelection();
        };

        // Starts with no band selected: addChildComponent() above doesn't force
        // a visibility state, so this establishes the strip's hidden default
        // (and the "no attachments" state) rather than leaving it visible at
        // whatever layoutBandStrip() position resized() first computes.
        rebuildBandStrip();

        for (auto* parameter : proc.getParameters()) {
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter)) {
                listenedParamIds.add(ranged->paramID);
                proc.apvts.addParameterListener(ranged->paramID, this);
            }
        }
        updateWorkflowControls();
        updateDetectionStyling();
        updateButtonStates();

        setResizable(true, true);
        setResizeLimits(892, 520, 2000, 1400);
        setSize(1132, 640);
        startTimerHz(30);
    }

    ~SpectralEditor() override {
        stopTimer();
        cancelPendingUpdate();
        for (const auto& id : activeSliderGestures)
            if (auto* parameter = proc.apvts.getParameter(id)) parameter->endChangeGesture();
        weightLayer.finishGesture();
        proc.workflow.finishUiEdit();
        for (const auto& id : listenedParamIds)
            proc.apvts.removeParameterListener(id, this);
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override {
        namespace pl = fixxy::palettelight;
        g.fillAll(pl::col(pl::window));

        auto r = getLocalBounds();
        auto header = r.removeFromTop(kHeaderH);
        auto bottom = r.removeFromBottom(kBottomH);
        auto rail = r.removeFromLeft(kRailW);
        auto sidebar = r.removeFromLeft(sidebarWidth());

        // Left rail: stone surface with a hairline separation.
        g.setColour(pl::col(pl::panel));
        g.fillRect(rail);
        g.setColour(pl::col(pl::hairline));
        g.drawVerticalLine(rail.getRight() - 1, static_cast<float>(rail.getY()),
                           static_cast<float>(rail.getBottom()));

        const bool hard = proc.apvts.getRawParameterValue("detectionMode")->load() > 0.5f;
        const float modeRadius = detectionPillBounds.getHeight()*0.5f;
        g.setColour(pl::col(pl::pillTrack));
        g.fillRoundedRectangle(detectionPillBounds, modeRadius);
        auto selectedMode = detectionPillBounds;
        selectedMode = hard ? selectedMode.removeFromRight(selectedMode.getWidth()*0.5f)
                            : selectedMode.removeFromLeft(selectedMode.getWidth()*0.5f);
        g.setColour(pl::col(pl::pillOnBg));
        g.fillRoundedRectangle(selectedMode, modeRadius);

        // Sidebar: stone surface (soothe3-style collapsible column), hairline
        // on its right edge — the boundary against the well (white/cream),
        // where `hairline` (numerically identical to `panel`) reads correctly.
        g.setColour(pl::col(pl::panel));
        g.fillRect(sidebar);
        g.setColour(pl::col(pl::hairline));
        g.drawVerticalLine(sidebar.getRight() - 1, static_cast<float>(sidebar.getY()),
                           static_cast<float>(sidebar.getBottom()));

        if (proc.workflow.isSidebarExpanded()) {
            // Sidebar group titles: flanking hairlines use `axis`, not
            // `hairline`, which is intentionally invisible on the stone panel.
            drawSidebarGroupTitle(g, stereoTitleRect, "stereo");
            drawSidebarGroupTitle(g, tiltTitleRect, "tilt");
            drawSidebarGroupTitle(g, scaleTitleRect, "scale");

            // msMode two-segment pill chrome, under its two real buttons.
            const bool ms = proc.apvts.getRawParameterValue("msMode")->load() > 0.5f;
            auto pillBounds = msModePillBounds;
            const float pr = pillBounds.getHeight() * 0.5f;
            g.setColour(pl::col(pl::pillTrack));
            g.fillRoundedRectangle(pillBounds, pr);
            auto half = pillBounds;
            auto activeHalf = ms ? half.removeFromRight(half.getWidth() * 0.5f)
                                 : half.removeFromLeft(half.getWidth() * 0.5f);
            juce::Path p;
            p.addRoundedRectangle(activeHalf.getX(), activeHalf.getY(), activeHalf.getWidth(),
                                  activeHalf.getHeight(), pr, pr, /*curveTopLeft=*/!ms,
                                  /*curveTopRight=*/ms, /*curveBottomLeft=*/!ms, /*curveBottomRight=*/ms);
            g.setColour(pl::col(pl::pillOnBg));
            g.fillPath(p);
        } else {
            drawCollapsedSidebarIndicators(g, sidebar);
        }

        // Header + bottom bar: cream with hairline dividers (no shadow).
        g.setColour(pl::col(pl::hairline));
        g.drawHorizontalLine(header.getBottom() - 1, 0.0f, static_cast<float>(getWidth()));
        g.drawHorizontalLine(bottom.getY(), 0.0f, static_cast<float>(getWidth()));

        g.setColour(pl::col(pl::textPrimary));
        g.setFont(fixxy::headerFont(16.0f));
        g.drawText(JucePlugin_Name, header.removeFromLeft(kRailW).reduced(2 * fixxy::metrics::kSpacing, 0),
                   juce::Justification::centredLeft);

        // A/B selector uses the same quiet ink pill as stereo mode. The copy
        // action beside it carries text, not an invented icon.
        const bool slotB = proc.workflow.activeSlot() == SpectralWorkflow::Slot::b;
        const float abRadius = abPillBounds.getHeight() * 0.5f;
        g.setColour(pl::col(pl::pillTrack));
        g.fillRoundedRectangle(abPillBounds, abRadius);
        auto activeAB = abPillBounds;
        activeAB = slotB ? activeAB.removeFromRight(activeAB.getWidth() * 0.5f)
                         : activeAB.removeFromLeft(activeAB.getWidth() * 0.5f);
        g.setColour(pl::col(pl::pillOnBg));
        g.fillRoundedRectangle(activeAB, abRadius);

    }

    void resized() override {
        using namespace fixxy::metrics;
        auto r = getLocalBounds();
        auto header = r.removeFromTop(kHeaderH);
        auto bottom = r.removeFromBottom(kBottomH);
        auto rail = r.removeFromLeft(kRailW);
        auto sidebar = r.removeFromLeft(sidebarWidth());
        layoutWorkflowToolbar(header);
        if (proc.workflow.isSidebarExpanded()) layoutSidebar(sidebar);

        auto well = r.reduced(kSpacing);
        display.setBounds(well);
        weightLayer.setBounds(well);

        const int helpWidth = juce::jmin(360, juce::jmax(260, well.getWidth() / 2));
        auto helpBounds = well;
        helpPanel.setBounds(helpBounds.removeFromTop(126).removeFromRight(helpWidth).reduced(8));

        const int stripHeight = well.getWidth() < 620 ? 76 : kBandStripH;
        auto strip = well.removeFromBottom(stripHeight).reduced(10, 6);
        bandStripPanel.setBounds(strip);
        layoutBandStrip(strip);

        // --- left rail: hero depth, detail, then attack|release row ---
        auto rc = rail.reduced(18, 16);
        auto detectorRow = rc.removeFromTop(26).withSizeKeepingCentre(140, 26);
        detectionPillBounds = detectorRow.toFloat();
        segSoft.setBounds(detectorRow.removeFromLeft(70));
        segHard.setBounds(detectorRow);
        rc.removeFromTop(8);
        const float knobScale = juce::jmin(1.0f, static_cast<float>(rc.getHeight() - 144) / 284.0f);
        const int depthSize = juce::roundToInt(144.0f * knobScale);
        const int detailSize = juce::roundToInt(88.0f * knobScale);
        const int timingSize = juce::roundToInt(52.0f * knobScale);
        const int contentH = depthSize + detailSize + timingSize + 144;
        rc.removeFromTop(juce::jmax(0, (rc.getHeight() - contentH) / 2));
        layoutKnob(knobs[0], rc.removeFromTop(depthSize + 40), depthSize);
        rc.removeFromTop(14);
        layoutKnob(knobs[1], rc.removeFromTop(detailSize + 38), detailSize);
        rc.removeFromTop(14);
        auto smallRow = rc.removeFromTop(timingSize + 38);
        layoutKnob(knobs[2], smallRow.removeFromLeft(smallRow.getWidth() / 2), timingSize);
        layoutKnob(knobs[3], smallRow, timingSize);

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
    static constexpr int kSidebarExpandedW = 132;
    static constexpr int kSidebarCollapsedW = 26;
    static constexpr int kBandStripH = 40;  // WeightBandLayer's band-edit strip, bottom of the well

    int sidebarWidth() const noexcept {
        return proc.workflow.isSidebarExpanded() ? kSidebarExpandedW : kSidebarCollapsedW;
    }

    void configureWorkflowToolbar() {
        namespace pl = fixxy::palettelight;
        for (auto* button : {&presetButton, &undoButton, &redoButton, &slotAButton,
                             &slotBButton, &copyButton, &sidebarButton, &helpButton,
                             &sidechainButton, &sidechainListenButton}) {
            addAndMakeVisible(*button);
            button->setColour(juce::TextButton::buttonColourId, pl::col(pl::panel));
            button->setColour(juce::TextButton::buttonOnColourId, pl::col(pl::panel));
            button->setColour(juce::TextButton::textColourOffId, pl::col(pl::textSecondary));
            button->setColour(juce::TextButton::textColourOnId, pl::col(pl::textPrimary));
        }

        presetButton.setTooltip("Load the factory baseline or open and save fixxy Spectral preset files.");
        presetButton.onClick = [this] { showPresetMenu(); };

        undoButton.setButtonText("undo");
        undoButton.setTooltip("Undo the active slot's last UI edit. Each A/B slot keeps its own history.");
        undoButton.onClick = [this] {
            proc.workflow.undo();
            updateWorkflowControls();
        };
        redoButton.setButtonText("redo");
        redoButton.setTooltip("Redo the active slot's last undone UI edit. Switching A/B preserves both histories.");
        redoButton.onClick = [this] {
            proc.workflow.redo();
            updateWorkflowControls();
        };

        for (auto* button : {&slotAButton, &slotBButton}) {
            button->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
            button->setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
        }
        slotAButton.setButtonText("A");
        slotAButton.setTooltip("Recall comparison slot A.");
        slotAButton.onClick = [this] {
            proc.workflow.selectSlot(SpectralWorkflow::Slot::a);
            updateWorkflowControls();
        };
        slotBButton.setButtonText("B");
        slotBButton.setTooltip("Recall comparison slot B.");
        slotBButton.onClick = [this] {
            proc.workflow.selectSlot(SpectralWorkflow::Slot::b);
            updateWorkflowControls();
        };

        copyButton.setTooltip("Copy the active comparison settings to the other slot.");
        copyButton.onClick = [this] {
            proc.workflow.copyActiveToOther();
            updateWorkflowControls();
        };

        sidebarButton.setTooltip("Show or hide the detailed stereo, tilt and scale controls.");
        sidebarButton.onClick = [this] {
            proc.workflow.setSidebarExpanded(!proc.workflow.isSidebarExpanded());
            updateWorkflowControls();
        };

        helpButton.setButtonText("?");
        helpButton.setTooltip("Show or hide context help.");
        helpButton.onClick = [this] {
            helpPanel.setVisible(!helpPanel.isVisible());
            updateWorkflowControls();
        };

        sidechainButton.setButtonText("SC");
        sidechainButton.setComponentID("externalSidechain");
        sidechainButton.setTooltip("Use the external sidechain to trigger reduction.");
        sidechainListenButton.setButtonText("listen");
        sidechainListenButton.setComponentID("sidechainListen");
        sidechainListenButton.setTooltip("Listen to the external input independently of SC detection.");
        for (auto* button : {&sidechainButton, &sidechainListenButton}) {
            button->setClickingTogglesState(true);
            button->setColour(juce::TextButton::buttonOnColourId, pl::col(pl::pillOnBg));
            button->setColour(juce::TextButton::textColourOnId, pl::col(pl::pillOnFg));
        }
        sidechainButton.onClick = [this] { setBoolGestured("externalSidechain", sidechainButton.getToggleState()); };
        sidechainListenButton.onClick = [this] { setBoolGestured("sidechainListen", sidechainListenButton.getToggleState()); };
        addAndMakeVisible(sidechainMeter);
        sidechainMeter.setComponentID("sidechainLevel");
        sidechainMeter.setTitle("Sidechain input level");
        sidechainMeter.setJustificationType(juce::Justification::centred);
        sidechainMeter.setFont(fixxy::labelFont(10.0f));
        sidechainMeter.setColour(juce::Label::textColourId, pl::col(pl::textPrimary));
        sidechainMeter.setLevelDb(proc.sidechainLevelDb.load(std::memory_order_relaxed));

        addChildComponent(helpPanel);
        helpPanel.setAlwaysOnTop(true);
    }

    void layoutWorkflowToolbar(juce::Rectangle<int> header) {
        header.removeFromLeft(kRailW);
        auto row = header.reduced(8, 9);
        presetButton.setBounds(row.removeFromLeft(150));
        row.removeFromLeft(6);
        undoButton.setBounds(row.removeFromLeft(46));
        row.removeFromLeft(4);
        redoButton.setBounds(row.removeFromLeft(46));
        row.removeFromLeft(8);

        auto ab = row.removeFromLeft(70);
        abPillBounds = ab.toFloat();
        slotAButton.setBounds(ab.removeFromLeft(ab.getWidth() / 2));
        slotBButton.setBounds(ab);
        row.removeFromLeft(6);
        copyButton.setBounds(row.removeFromLeft(58));
        row.removeFromLeft(8);
        sidebarButton.setBounds(row.removeFromLeft(72));
        row.removeFromLeft(6);
        helpButton.setBounds(row.removeFromLeft(30));
        row.removeFromLeft(8);
        sidechainButton.setBounds(row.removeFromLeft(34));
        row.removeFromLeft(4);
        sidechainListenButton.setBounds(row.removeFromLeft(46));
        row.removeFromLeft(6);
        sidechainMeter.setBounds(row.removeFromLeft(52));
    }

    void showPresetMenu() {
        juce::PopupMenu menu;
        menu.addItem(1, "Factory baseline");
        menu.addSeparator();
        menu.addItem(2, "Open preset file...");
        menu.addItem(3, "Save preset as...");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presetButton),
                           [safe = juce::Component::SafePointer<SpectralEditor>(this)](int choice) {
            if (safe == nullptr) return;
            if (choice == 1) {
                safe->proc.workflow.loadFactoryBaseline();
                safe->updateWorkflowControls();
            } else if (choice == 2) {
                safe->beginOpenPreset();
            } else if (choice == 3) {
                safe->beginSavePreset();
            }
        });
    }

    void beginOpenPreset() {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Open fixxy Spectral preset", presetDirectory(),
            juce::String("*") + spectralstate::kPresetExtension + ";*"
                + spectralstate::kLegacyPresetExtension + ";*.xml");
        fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                     | juce::FileBrowserComponent::canSelectFiles,
                                 [safe = juce::Component::SafePointer<SpectralEditor>(this)](
                                     const juce::FileChooser& chooser) {
            if (safe == nullptr) return;
            const auto file = chooser.getResult();
            if (file != juce::File()) {
                safe->lastPresetDirectory = file.getParentDirectory();
                const auto result = safe->proc.workflow.loadPreset(file);
                if (result.failed()) safe->showWorkflowError("Preset not loaded", result);
                safe->updateWorkflowControls();
            }
            safe->fileChooser.reset();
        });
    }

    void beginSavePreset() {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Save fixxy Spectral preset",
            presetDirectory().getChildFile(juce::String("fixxy Spectral") + spectralstate::kPresetExtension),
            juce::String("*") + spectralstate::kPresetExtension);
        fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                     | juce::FileBrowserComponent::canSelectFiles
                                     | juce::FileBrowserComponent::warnAboutOverwriting,
                                 [safe = juce::Component::SafePointer<SpectralEditor>(this)](
                                     const juce::FileChooser& chooser) {
            if (safe == nullptr) return;
            auto file = chooser.getResult();
            if (file != juce::File()) {
                if (!file.hasFileExtension(spectralstate::kPresetExtension))
                    file = file.withFileExtension(spectralstate::kPresetExtension);
                safe->lastPresetDirectory = file.getParentDirectory();
                const auto result = safe->proc.workflow.savePreset(file);
                if (result.failed()) safe->showWorkflowError("Preset not saved", result);
                safe->updateWorkflowControls();
            }
            safe->fileChooser.reset();
        });
    }

    juce::File presetDirectory() const {
        return lastPresetDirectory.isDirectory()
                   ? lastPresetDirectory
                   : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    }

    void showWorkflowError(const juce::String& title, const juce::Result& result) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               title, result.getErrorMessage());
    }

    void updateWorkflowControls() {
        namespace pl = fixxy::palettelight;
        const auto observedRevision = proc.workflow.revision();
        const bool slotB = proc.workflow.activeSlot() == SpectralWorkflow::Slot::b;
        slotAButton.setColour(juce::TextButton::textColourOffId,
                              pl::col(slotB ? pl::pillFgOff : pl::pillOnFg));
        slotBButton.setColour(juce::TextButton::textColourOffId,
                              pl::col(slotB ? pl::pillOnFg : pl::pillFgOff));
        copyButton.setButtonText(slotB ? "B to A" : "A to B");
        undoButton.setEnabled(proc.workflow.canUndo());
        redoButton.setEnabled(proc.workflow.canRedo());
        presetButton.setButtonText(proc.workflow.currentPresetName()
                                   + (proc.workflow.isCurrentPresetDirty() ? " *" : ""));
        const bool expanded = proc.workflow.isSidebarExpanded();
        const bool layoutChanged = expanded != lastSidebarExpanded;
        lastSidebarExpanded = expanded;
        sidebarButton.setButtonText(expanded ? "details <" : "details >");
        updateSidebarVisibility(expanded);
        helpButton.setColour(juce::TextButton::buttonColourId,
                             pl::col(helpPanel.isVisible() ? pl::pillOnBg : pl::panel));
        helpButton.setColour(juce::TextButton::textColourOffId,
                             pl::col(helpPanel.isVisible() ? pl::pillOnFg : pl::textSecondary));
        helpPanel.selectedBand = selectedBand;
        helpPanel.repaint();
        lastWorkflowRevision = observedRevision;
        if (layoutChanged) resized();
        repaint();
    }

    void updateSidebarVisibility(bool visible) {
        segLR.setVisible(visible);
        segMS.setVisible(visible);
        for (auto& chip : sidebarChips) {
            chip.name.setVisible(visible);
            chip.slider.setVisible(visible);
        }
    }

    bool parameterDiffersFromDefault(const char* id) const {
        auto* parameter = proc.apvts.getParameter(id);
        auto* value = proc.apvts.getRawParameterValue(id);
        if (parameter == nullptr || value == nullptr) return false;
        const auto defaultValue = parameter->convertFrom0to1(parameter->getDefaultValue());
        return std::abs(value->load() - defaultValue) > 1.0e-5f;
    }

    void drawCollapsedSidebarIndicators(juce::Graphics& g, juce::Rectangle<int> sidebar) {
        namespace pl = fixxy::palettelight;
        const std::array<bool, 3> changed{{
            parameterDiffersFromDefault("msMode") || parameterDiffersFromDefault("linkPct")
                || parameterDiffersFromDefault("focus"),
            parameterDiffersFromDefault("tiltDetail") || parameterDiffersFromDefault("tiltAttack")
                || parameterDiffersFromDefault("tiltRelease"),
            parameterDiffersFromDefault("maxCut") || parameterDiffersFromDefault("wetTrim")}};
        auto indicator = sidebar.removeFromTop(110).removeFromBottom(66);
        for (const bool active : changed) {
            const auto dot = indicator.removeFromTop(14).withSizeKeepingCentre(6, 6).toFloat();
            g.setColour(pl::col(active ? pl::controlFill : pl::tick));
            g.fillEllipse(dot);
            indicator.removeFromTop(8);
        }
    }

    struct KnobDef {
        const char* paramId;
        const char* title;
        bool action;  // true -> ember fill (the amount the processor acts by)
        bool hero;    // true -> larger, ink uppercase-ish name
        const char* tooltip;
    };
    static constexpr std::size_t kNumKnobs = 4;
    static inline const std::array<KnobDef, kNumKnobs> kKnobDefs{{
        {"depth", "DEPTH", true, true, "Set the overall strength of resonance suppression."},
        {"detail", "detail", false, false, "Adjust how selectively resonances are detected and suppressed."},
        {"attack", "attack", false, false, "Set how quickly suppression responds to a resonance, in ms."},
        {"release", "release", false, false, "Set how quickly suppression recovers after a resonance, in ms."},
    }};

    struct ChipDef { const char* paramId; const char* title; };
    static constexpr std::size_t kNumChips = 2;
    static inline const std::array<ChipDef, kNumChips> kChipDefs{{
        {"mix", "mix"},
        {"output", "out"},
    }};

    // Sidebar chips: a parallel array to ChipDef/kChipDefs (kept separate
    // rather than reusing it) because these also need a bipolar flag, for
    // which sliders get setDoubleClickReturnValue(true, 0.0) (reset-to-centre)
    // in the constructor.
    struct SidebarChipDef { const char* paramId; const char* title; bool bipolar; const char* tooltip; };
    static constexpr std::size_t kNumSidebarChips = 7;
    static inline const std::array<SidebarChipDef, kNumSidebarChips> kSidebarChipDefs{{
        {"linkPct", "link", false, "Link channel detection: 100% applies shared reduction; 0% detects independently."},
        {"focus", "focus", true, "Bias processing between the two channels. Double-click to centre."},
        {"tiltDetail", "detail", true, "Positive values increase suppression strength toward high frequencies. Double-click to reset."},
        {"tiltAttack", "attack", true, "Positive values make attack slower toward high frequencies. Double-click to reset."},
        {"tiltRelease", "release", true, "Positive values make release slower toward high frequencies. Double-click to reset."},
        {"maxCut", "max cut", false, "Limit the maximum reduction in dB; off removes this limit."},
        {"wetTrim", "wet trim", false, "Adjust only the processed signal level before the dry/wet blend."},
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

    struct HelpPanel : juce::Component {
        int selectedBand = -1;

        HelpPanel() { setInterceptsMouseClicks(false, false); }

        void paint(juce::Graphics& g) override {
            namespace pl = fixxy::palettelight;
            const auto bounds = getLocalBounds().toFloat();
            g.setColour(pl::col(pl::panel).withAlpha(0.96f));
            g.fillRoundedRectangle(bounds, fixxy::metrics::kCornerCard);
            g.setColour(pl::col(pl::axis));
            g.drawRoundedRectangle(bounds.reduced(0.5f), fixxy::metrics::kCornerCard, 1.0f);

            auto text = getLocalBounds().reduced(14, 10);
            g.setColour(pl::col(pl::textPrimary));
            g.setFont(fixxy::headerFont(14.0f));
            g.drawText(selectedBand >= 0 ? "Band " + juce::String(selectedBand + 1)
                                         : "Quick start",
                       text.removeFromTop(22), juce::Justification::centredLeft);
            g.setColour(pl::col(pl::textSecondary));
            g.setFont(fixxy::labelFont(fixxy::metrics::kFontLabel));
            const auto guidance = selectedBand >= 0
                ? juce::String("Enabled bypasses this band without losing it. Shape, frequency, "
                               "depth and Q tune its contribution; delete frees the slot.")
                : juce::String("Raise depth until the effect is clear, refine detail, then use delta "
                               "to hear what is removed. Double-click the graph to add a band and "
                               "finish by comparing with bypass.");
            g.drawFittedText(guidance, text, juce::Justification::topLeft, 4, 0.86f);
        }
    };

    void layoutKnob(Knob& k, juce::Rectangle<int> cell, int knobPx) {
        k.name.setBounds(cell.removeFromTop(18));
        const int w = juce::jmin(cell.getWidth(), juce::jmax(knobPx, 60));
        const int h = juce::jmin(cell.getHeight(), knobPx + 20);
        k.slider.setBounds(cell.withSizeKeepingCentre(w, h));
    }

    void layoutChip(Chip& c, juce::Rectangle<int> cell, int chipH, int labelWidth = 34) {
        auto row = cell.withSizeKeepingCentre(cell.getWidth(), chipH);
        c.name.setBounds(row.removeFromLeft(labelWidth));
        c.slider.setBounds(row);
    }

    // Soothe-style "—stereo—" group title: centred muted uppercase text
    // flanked by two short axis-coloured hairlines.
    void drawSidebarGroupTitle(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text) {
        namespace pl = fixxy::palettelight;
        g.setFont(fixxy::labelFont(fixxy::metrics::kFontSmall));
        auto label = text.toUpperCase();
        const int textW = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), label) + 4;
        const int cx = r.getCentreX(), cy = r.getCentreY();
        const int halfText = textW / 2;
        const int gap = 6;
        g.setColour(pl::col(pl::axis));
        if (cx - halfText - gap > r.getX())
            g.drawHorizontalLine(cy, static_cast<float>(r.getX()), static_cast<float>(cx - halfText - gap));
        if (cx + halfText + gap < r.getRight())
            g.drawHorizontalLine(cy, static_cast<float>(cx + halfText + gap), static_cast<float>(r.getRight()));
        g.setColour(pl::col(pl::textMuted));
        g.drawText(label, r, juce::Justification::centred, false);
    }

    // Top-anchored sidebar stack: msMode pill + link/focus ("stereo"), tilt
    // detail/attack/release ("tilt"), max cut/wet trim ("scale"). Computes
    // every title rect, the pill bounds, and every chip's bounds; blank stone
    // remains below (soothe3's sidebar doesn't vertically centre).
    void layoutSidebar(juce::Rectangle<int> sidebar) {
        auto content = sidebar.reduced(10, 0);
        content.removeFromTop(16);  // top pad

        stereoTitleRect = content.removeFromTop(16);
        content.removeFromTop(6);
        auto pillCell = content.removeFromTop(24);
        msModePillBounds = pillCell.toFloat();
        segLR.setBounds(pillCell.removeFromLeft(pillCell.getWidth() / 2));
        segMS.setBounds(pillCell);
        content.removeFromTop(6);
        layoutChip(sidebarChips[0], content.removeFromTop(20), 20, 52);  // link
        content.removeFromTop(6);
        layoutChip(sidebarChips[1], content.removeFromTop(20), 20, 52);  // focus
        content.removeFromTop(14);

        tiltTitleRect = content.removeFromTop(16);
        content.removeFromTop(6);
        layoutChip(sidebarChips[2], content.removeFromTop(20), 20, 52);  // detail
        content.removeFromTop(6);
        layoutChip(sidebarChips[3], content.removeFromTop(20), 20, 52);  // attack
        content.removeFromTop(6);
        layoutChip(sidebarChips[4], content.removeFromTop(20), 20, 52);  // release
        content.removeFromTop(14);

        scaleTitleRect = content.removeFromTop(16);
        content.removeFromTop(6);
        layoutChip(sidebarChips[5], content.removeFromTop(20), 20, 52);  // max cut
        content.removeFromTop(6);
        layoutChip(sidebarChips[6], content.removeFromTop(20), 20, 52);  // wet trim
    }

    // 0-based, mirrors PluginProcessor.cpp's wbandID()/WeightBandLayer.h's copy
    // exactly ("wband" + i + suffix, i = 0..7).
    static juce::String wbandID(int i, const char* suffix) {
        return "wband" + juce::String(i) + suffix;
    }

    void layoutBandStrip(juce::Rectangle<int> strip) {
        const auto origin = strip.getPosition();
        const bool compact = strip.getHeight() > 40;
        auto values = compact ? strip.removeFromBottom(28) : juce::Rectangle<int>{};
        if (compact) strip = strip.removeFromTop(28);
        bandStripPanel.badgeBounds = strip.removeFromLeft(strip.getHeight()).reduced(4)
                                         .translated(-origin.x, -origin.y).toFloat();
        strip.removeFromLeft(8);
        bandOnButton.setBounds(strip.removeFromLeft(62));
        strip.removeFromLeft(6);
        bandTypeBox.setBounds(strip.removeFromLeft(96));
        strip.removeFromLeft(10);
        if (compact) {
            bandDeleteButton.setBounds(strip.removeFromRight(60));
            const int cellWidth = (values.getWidth() - 20) / 3;
            layoutChip(bandFreqChip, values.removeFromLeft(cellWidth), 28);
            values.removeFromLeft(10);
            layoutChip(bandGainChip, values.removeFromLeft(cellWidth), 28);
            values.removeFromLeft(10);
            layoutChip(bandQChip, values, 28);
            return;
        }
        layoutChip(bandFreqChip, strip.removeFromLeft(76), strip.getHeight());
        strip.removeFromLeft(6);
        layoutChip(bandGainChip, strip.removeFromLeft(76), strip.getHeight());
        strip.removeFromLeft(6);
        layoutChip(bandQChip, strip.removeFromLeft(64), strip.getHeight());
        strip.removeFromRight(4);
        bandDeleteButton.setBounds(strip.removeFromRight(52));
    }

    // Selection changed (WeightBandLayer::onSelectionChanged) -> show/hide the
    // strip and re-point every attachment at the newly selected band. Mirrors
    // EqEditor::rebuildAttachments()'s pattern (including the Type-param
    // listener re-pointing for the gain-chip disable below).
    void rebuildBandStrip() {
        const bool visible = selectedBand >= 0;
        bandStripPanel.selectedBand = selectedBand;
        bandStripPanel.setVisible(visible);
        bandStripPanel.repaint();
        bandOnButton.setVisible(visible);
        bandTypeBox.setVisible(visible);
        bandFreqChip.name.setVisible(visible);
        bandFreqChip.slider.setVisible(visible);
        bandGainChip.name.setVisible(visible);
        bandGainChip.slider.setVisible(visible);
        bandQChip.name.setVisible(visible);
        bandQChip.slider.setVisible(visible);
        bandDeleteButton.setVisible(visible);

        if (!visible) {
            bandFreqChip.attachment.reset();
            bandGainChip.attachment.reset();
            bandQChip.attachment.reset();
            return;
        }

        using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        bandFreqChip.attachment = std::make_unique<SliderAttachment>(
            proc.apvts, wbandID(selectedBand, "Freq"), bandFreqChip.slider);
        bandGainChip.attachment = std::make_unique<SliderAttachment>(
            proc.apvts, wbandID(selectedBand, "Gain"), bandGainChip.slider);
        bandQChip.attachment =
            std::make_unique<SliderAttachment>(proc.apvts, wbandID(selectedBand, "Q"), bandQChip.slider);
        bindSliderGestures(bandFreqChip.slider, wbandID(selectedBand, "Freq"));
        bindSliderGestures(bandGainChip.slider, wbandID(selectedBand, "Gain"));
        bindSliderGestures(bandQChip.slider, wbandID(selectedBand, "Q"));
        updateButtonStates();

        bandOnButton.setColour(juce::ToggleButton::tickColourId,
                               fixxy::palettelight::bandColour(selectedBand));

        updateGainChipEnablement();
    }

    // Gain chip means nothing for gain-less types (freq/Q-only weight bands) —
    // grey it out, same reasoning as EqEditor's slope-disable handling.
    void updateGainChipEnablement() {
        if (selectedBand < 0) return;
        const int type =
            static_cast<int>(proc.apvts.getRawParameterValue(wbandID(selectedBand, "Type"))->load());
        bandGainChip.slider.setEnabled(svfcoeffs::isGainType(static_cast<svfcoeffs::Type>(type)));
    }

    // Reads msMode and colours segLR/segMS's text so the active segment reads
    // cream-on-ink and the idle one reads muted; the pill fill itself is
    // repainted from paint() (see the msMode pill block there). Called once
    // from the constructor for the initial state, and from handleAsyncUpdate()
    // whenever msMode changes via undo/preset load/host automation.
    void updateMsModeStyling() {
        namespace pl = fixxy::palettelight;
        const bool ms = proc.apvts.getRawParameterValue("msMode")->load() > 0.5f;
        segLR.setColour(juce::TextButton::textColourOffId, pl::col(ms ? pl::pillFgOff : pl::pillOnFg));
        segMS.setColour(juce::TextButton::textColourOffId, pl::col(ms ? pl::pillOnFg : pl::pillFgOff));
        repaint();
    }

    void updateDetectionStyling() {
        namespace pl = fixxy::palettelight;
        const bool hard = proc.apvts.getRawParameterValue("detectionMode")->load() > 0.5f;
        segSoft.setColour(juce::TextButton::textColourOffId, pl::col(hard ? pl::pillFgOff : pl::pillOnFg));
        segHard.setColour(juce::TextButton::textColourOffId, pl::col(hard ? pl::pillOnFg : pl::pillFgOff));
        auto& tilt = sidebarChips[2];
        tilt.slider.setEnabled(!hard);
        tilt.name.setEnabled(!hard);
        tilt.slider.setTooltip(hard ? "Detail tilt affects Soft sensitivity only. Detail still sets the processing width in Hard."
                                   : kSidebarChipDefs[2].tooltip);
        repaint();
    }

    void bindSliderGestures(juce::Slider& slider, const juce::String& id) {
        // JUCE calls these after attachment gesture notifications have returned.
        // Do not move journal work into onValueChange or a parameter listener.
        slider.onDragStart = [this, id] {
            proc.workflow.beginUiEdit();
            proc.workflow.markUiParameter(id);
            activeSliderGestures.addIfNotAlreadyThere(id);
        };
        slider.onDragEnd = [this, id] {
            activeSliderGestures.removeString(id);
            proc.workflow.endUiEdit();
            if (pendingBandSelection.has_value()) triggerAsyncUpdate();
        };
    }

    bool hasActiveBandSliderGesture() const {
        for (const auto& id : activeSliderGestures)
            if (id.startsWith("wband")) return true;
        return false;
    }

    void updateButtonStates() {
        const auto enabled = [this](const juce::String& id) { return proc.apvts.getRawParameterValue(id)->load() > 0.5f; };
        bypassButton.setToggleState(enabled("bypass"), juce::dontSendNotification);
        deltaButton.setToggleState(enabled("delta"), juce::dontSendNotification);
        sidechainButton.setToggleState(enabled("externalSidechain"), juce::dontSendNotification);
        sidechainListenButton.setToggleState(enabled("sidechainListen"), juce::dontSendNotification);
        if (selectedBand >= 0) {
            bandOnButton.setToggleState(enabled(wbandID(selectedBand, "On")), juce::dontSendNotification);
            bandTypeBox.setSelectedId(juce::roundToInt(proc.apvts.getRawParameterValue(wbandID(selectedBand, "Type"))->load()) + 1,
                                     juce::dontSendNotification);
        }
    }

    void setParameterGestured(const juce::String& id, float value) {
        if (auto* p = proc.apvts.getParameter(id)) {
            proc.workflow.beginUiEdit();
            proc.workflow.markUiParameter(id);
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(value));
            p->endChangeGesture();
            proc.workflow.endUiEdit();
        }
    }

    void setBoolGestured(const juce::String& id, bool on) {
        setParameterGestured(id, on ? 1.0f : 0.0f);
    }

    // Type param can change from the strip's own combo (ComboBoxAttachment),
    // a new band selection (handled directly in rebuildBandStrip() above), or
    // host automation -- which, per APVTS::Listener's contract, may call back
    // on a non-message thread, so this only flags work; handleAsyncUpdate()
    // does the actual GUI touch. Mirrors EqEditor's identical marshalling.
    void parameterChanged(const juce::String&, float) override { triggerAsyncUpdate(); }

    void handleAsyncUpdate() override {
        if (pendingBandSelection.has_value() && !hasActiveBandSliderGesture()) {
            selectedBand = *pendingBandSelection;
            pendingBandSelection.reset();
            rebuildBandStrip();
            repaint();
        }
        updateGainChipEnablement();
        updateMsModeStyling();
        updateDetectionStyling();
        updateButtonStates();
        updateWorkflowControls();
    }

    void timerCallback() override {
        // Host restores can change workflow metadata without changing a
        // parameter, so APVTS callbacks alone cannot refresh this chrome.
        if (proc.workflow.revision() != lastWorkflowRevision)
            updateWorkflowControls();
        sidechainMeter.setLevelDb(proc.sidechainLevelDb.load(std::memory_order_relaxed));
    }

    // Choice order must match kWeightTypeChoices in PluginProcessor.cpp
    // exactly (bell/low shelf/... == svfcoeffs::Type order); this list is just
    // the combo's DISPLAY text (ComboBoxAttachment binds by index, not text).
    static inline const juce::StringArray kWeightTypeChoices{
        "Bell", "Low Shelf", "High Shelf", "Tilt", "Notch", "Bandpass", "Lowpass", "Highpass"};

    SpectralProcessor& proc;
    fixxy::FixxyLookAndFeelLight lnf;  // must outlive the children below
    juce::TooltipWindow tooltipWindow{this, 600};

    juce::TextButton presetButton, undoButton, redoButton;
    juce::TextButton slotAButton, slotBButton, copyButton, sidebarButton, helpButton;
    juce::TextButton sidechainButton, sidechainListenButton;
    struct SidechainMeter final : juce::Label {
        void setLevelDb(float value) {
            if (value == levelDb) return;
            levelDb = value;
            setText(value <= -100.0f ? juce::String::fromUTF8("\xe2\x88\x92\xe2\x88\x9e")
                                    : juce::String(value, 1) + " dB", juce::dontSendNotification);
            setTooltip("External sidechain input: " + getText() + "FS");
            repaint();
        }
        void paint(juce::Graphics& g) override {
            namespace pl = fixxy::palettelight;
            const auto bounds = getLocalBounds().toFloat();
            g.setColour(pl::col(pl::pillTrack));
            g.fillRoundedRectangle(bounds, 3.0f);
            const float fraction = juce::jlimit(0.0f, 1.0f, (levelDb + 60.0f) / 60.0f);
            g.setColour(pl::col(pl::controlFill).withAlpha(0.35f));
            g.fillRoundedRectangle(bounds.withWidth(bounds.getWidth() * fraction), 3.0f);
            juce::Label::paint(g);
        }
        float levelDb = -101.0f;
    } sidechainMeter;
    juce::Rectangle<float> abPillBounds;
    HelpPanel helpPanel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::File lastPresetDirectory;
    juce::StringArray listenedParamIds;
    juce::StringArray activeSliderGestures;
    std::optional<int> pendingBandSelection;
    std::uint64_t lastWorkflowRevision = 0;
    bool lastSidebarExpanded = true;

    SpectralAnalyzerView display;
    WeightBandLayer weightLayer;

    std::array<Knob, kNumKnobs> knobs;
    std::array<Chip, kNumChips> chips;

    // --- sidebar column (soothe3-style [L/R|M/S] pill + link/focus/tilt/
    // scale chips) ---
    juce::TextButton segLR, segMS;
    juce::TextButton segSoft, segHard;
    juce::Rectangle<float> detectionPillBounds;
    juce::Rectangle<float> msModePillBounds;
    juce::Rectangle<int> stereoTitleRect, tiltTitleRect, scaleTitleRect;
    std::array<Chip, kNumSidebarChips> sidebarChips;

    juce::ToggleButton bypassButton, deltaButton;

    // --- band-edit strip (visible only while WeightBandLayer reports a
    // selection) ---
    int selectedBand = -1;
    // A foreground child is required: the opaque analyzer covers parent paint.
    struct BandStripPanel : juce::Component {
        int selectedBand = -1;
        juce::Rectangle<float> badgeBounds;

        void paint(juce::Graphics& g) override {
            namespace pl = fixxy::palettelight;
            const auto bounds = getLocalBounds().toFloat();
            g.setColour(pl::col(pl::panel));
            g.fillRoundedRectangle(bounds, fixxy::metrics::kCornerCard);
            g.setColour(pl::col(pl::axis));
            g.drawRoundedRectangle(bounds.reduced(0.5f), fixxy::metrics::kCornerCard, 1.0f);
            g.setColour(pl::bandColour(selectedBand));
            g.fillEllipse(badgeBounds);
            g.setColour(pl::col(pl::textPrimary));
            g.setFont(fixxy::labelFont(fixxy::metrics::kFontLabel).boldened());
            g.drawText(juce::String(selectedBand + 1), badgeBounds.toNearestInt(),
                       juce::Justification::centred);
        }
    } bandStripPanel;
    juce::ToggleButton bandOnButton;
    juce::ComboBox bandTypeBox;
    Chip bandFreqChip, bandGainChip, bandQChip;
    juce::TextButton bandDeleteButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralEditor)
};
