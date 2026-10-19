#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <functional>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "dsp/BandResponse.h"
#include "dsp/SpectralWeight.h"
#include "plugins/FixxyLookAndFeel.h"
#include "plugins/spectral/SpectralWorkflow.h"
#include "plugins/spectral/ui/SpectralDisplayMap.h"

/**
    The soothe3-style weight-band editing layer: sits over SpectralAnalyzerView
    in the display well (same bounds, added after it so it's on top) and owns
    ALL interaction with the up to kMaxWeightBands "wband" params plus the
    [rangeLo, rangeHi] window.

    DELIBERATELY a fresh copy of DraggerLayer.h's selection/ghost/gesture
    lifecycle (src/plugins/eq/ui/DraggerLayer.h), not a shared base class —
    see that file's header comment for the pattern this mirrors. Two real
    differences from the EQ version: (1) there is no wband Slope param, so
    every slope-related branch DraggerLayer has (context menu, cmd+wheel,
    kLowpass/kHighpass special-casing) is simply absent here; Low/High-pass
    weight bands use spectralweight::BandWeightEval's fixed 12 dB/oct single-
    section convention, not the EQ's slope cascade. (2) this class ALSO draws
    (CurveView.h's role in the EQ) — the composite weight curve is cheap
    enough (kMaxWeightBands=8 bands x kNumPoints) that splitting it into a
    second file would be pure ceremony.

    COMPOSITE CURVE, single-source: composite(f) = gateDb(f, rangeLo, rangeHi)
    + the dB SUM of every ENABLED band's magnitude response at f, both from
    dsp/SpectralWeight.h — the SAME header SpectralNode's audio-thread weight
    table consumes (or will, once wired in). Each band's magnitude comes from
    spectralweight::BandWeightEval — the analog-SVF SectionRecipe prototype
    (svfcoeffs / BandResponse.h), NOT the EQ's matched-biquad path: weight
    bands aren't real audio filters, they're a dB bias curve, and the node's
    weight table is proto-verified against that analog recipe, not the
    matched-digital one CurveView.h draws for actual EQ bands. Using the
    matched path here was a single-source bug (diverges up to a few dB near
    Nyquist) — fixed by pulling BandWeightEval into dsp/SpectralWeight.h so
    this view and the node share the identical evaluator. No filter math is
    re-derived here. The gate is added ONCE to the total, not per band, since
    spectralweight::gateDb() is a property of the RANGE window, not of any
    one band.

    Curve recompute is dirty-flag + 30 Hz-timer driven (mirrors CurveView.h):
    an APVTS listener flags `dirty` on any wband/range change (with the same
    normalized-delta echo guard DraggerLayer uses on its continuous params, so
    reading back a value we just wrote doesn't spam extra recomputes), and the
    timer drains it into a cached juce::Path that paint() only strokes. Handle
    (dot/pin) POSITIONS are never cached — like DraggerLayer, paint() and every
    mouse handler read the CURRENT apvts values and recompute x/y on the spot,
    which is what keeps a handle glued exactly to the curve as host automation
    moves it, without a second cache to keep in sync.
*/
class WeightBandLayer : public juce::Component,
                         private juce::Timer,
                         private juce::AudioProcessorValueTreeState::Listener {
public:
    static constexpr int kNumWeightBands = spectralweight::kMaxWeightBands;

    /** Fired whenever a click/double-click/delete changes which band is
        selected. -1 = no selection. SpectralEditor's band-edit strip
        subscribes to drive its own visibility + attachments. */
    std::function<void(int)> onSelectionChanged;

    WeightBandLayer(juce::AudioProcessorValueTreeState& state, const std::atomic<double>& hostFs,
                    SpectralWorkflow& workflowToUse)
        : apvts(state), hostSampleRate(hostFs), workflow(workflowToUse),
          lastPresenceMask(workflow.bandPresenceMask()) {
        setWantsKeyboardFocus(true);

        const float logMin = std::log10(spectralui::DisplayMap::kMinFreq);
        const float logMax = std::log10(spectralui::DisplayMap::kMaxFreq);
        for (int p = 0; p < kNumPoints; ++p) {
            const float t = static_cast<float>(p) / static_cast<float>(kNumPoints - 1);
            freqPoints[static_cast<std::size_t>(p)] = std::pow(10.0f, logMin + t * (logMax - logMin));
        }

        for (int i = 0; i < kNumWeightBands; ++i)
            for (const char* suffix : kBandSuffixes)
                apvts.addParameterListener(wbandID(i, suffix), this);
        apvts.addParameterListener("rangeLo", this);
        apvts.addParameterListener("rangeHi", this);

        for (int i = 0; i < kNumWeightBands; ++i) {
            const auto ci = static_cast<std::size_t>(i);
            lastFreqNorm[ci] = normOf(wbandID(i, "Freq"));
            lastGainNorm[ci] = normOf(wbandID(i, "Gain"));
            lastQNorm[ci] = normOf(wbandID(i, "Q"));
        }
        lastRangeLoNorm = normOf("rangeLo");
        lastRangeHiNorm = normOf("rangeHi");

        recomputeCurve();  // harmless zero-bounds seed; resized() recomputes with real bounds
        startTimerHz(30);
    }

    ~WeightBandLayer() override {
        finishGesture();
        for (int i = 0; i < kNumWeightBands; ++i)
            for (const char* suffix : kBandSuffixes)
                apvts.removeParameterListener(wbandID(i, suffix), this);
        apvts.removeParameterListener("rangeLo", this);
        apvts.removeParameterListener("rangeHi", this);
    }

    /** Clears only the editor selection; lifecycle changes remain the workflow's
        responsibility so retained bypass is never confused with deletion. */
    void clearSelection() { selectBand(-1); }

    void resized() override {
        auto b = getLocalBounds().toFloat().reduced(static_cast<float>(kInset));
        map.setBounds(b.getX(), b.getY(), b.getWidth(), b.getHeight());
        recomputeCurve();
        repaint();
    }

    void paint(juce::Graphics& g) override {
        namespace pl = fixxy::palettelight;

        // Composite weight curve: the ink token (dataSum), per docs/issues.md
        // DESIGN-DIRECTION — the weighting curve must be the highest-contrast
        // mark on paper.
        g.setColour(pl::col(pl::dataSum));
        g.strokePath(curvePath, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));

        drawRangePins(g);

        bool anyPresent = false;
        for (int i = 0; i < kNumWeightBands; ++i) {
            if (!workflow.isBandPresent(i)) continue;
            const BandSnapshot s = snapshot(i);
            anyPresent = true;
            drawDot(g, i, handleCenter(s), i == selectedBand, s.on);
        }

        if (!anyPresent && !hasGhost) {
            g.setColour(pl::col(pl::textMuted).withAlpha(0.6f));
            g.setFont(fixxy::labelFont(fixxy::metrics::kFontLabel));
            g.drawText("Double-click to add a weight band", getLocalBounds(),
                       juce::Justification::centred);
        }

        if (hasGhost)
            drawDot(g, ghostBandIdx, ghostCenter, false, true, 0.35f);
    }

    void mouseMove(const juce::MouseEvent& e) override {
        const int hitDot = findHandleAt(e.position);
        const int hitPin = hitDot < 0 ? findPinAt(e.position.x) : -1;
        setMouseCursor(hitDot >= 0 ? juce::MouseCursor::PointingHandCursor
                       : hitPin >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                                     : juce::MouseCursor::NormalCursor);
        if (hitPin != hoveredPin) {
            hoveredPin = hitPin;
            repaint();
        }
        updateGhost(hitDot, hitPin, e.position);
    }

    void mouseExit(const juce::MouseEvent&) override {
        setMouseCursor(juce::MouseCursor::NormalCursor);
        if (hoveredPin != -1) {
            hoveredPin = -1;
            repaint();
        }
        if (hasGhost) {
            hasGhost = false;
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& e) override {
        grabKeyboardFocus();

        const int hitDot = findHandleAt(e.position);

        if (e.mods.isPopupMenu()) {
            if (hitDot >= 0) {
                selectBand(hitDot);
                showContextMenu(hitDot);
            }
            return;
        }

        if (hitDot >= 0) {
            selectBand(hitDot);
            const BandSnapshot s = snapshot(hitDot);
            dotDrag.band = hitDot;
            dotDrag.gestureStarted = false;
            dotDrag.isGain = svfcoeffs::isGainType(static_cast<svfcoeffs::Type>(s.type));
            return;
        }

        const int hitPin = findPinAt(e.position.x);
        if (hitPin >= 0) {
            pinDrag.pin = hitPin;
            pinDrag.gestureStarted = false;
            return;
        }

        selectBand(-1);
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (dotDrag.band >= 0) {
            auto* freqParam = apvts.getParameter(wbandID(dotDrag.band, "Freq"));
            auto* gainParam = dotDrag.isGain ? apvts.getParameter(wbandID(dotDrag.band, "Gain")) : nullptr;
            if (freqParam == nullptr) return;

            if (!dotDrag.gestureStarted) {
                workflow.beginUiEdit("Move band");
                workflow.markUiParameter(wbandID(dotDrag.band, "Freq"));
                if (gainParam != nullptr) workflow.markUiParameter(wbandID(dotDrag.band, "Gain"));
                freqParam->beginChangeGesture();
                if (gainParam != nullptr) gainParam->beginChangeGesture();
                dotDrag.gestureStarted = true;
            }

            freqParam->setValueNotifyingHost(freqParam->convertTo0to1(map.xToFreq(e.position.x)));
            if (gainParam != nullptr)
                gainParam->setValueNotifyingHost(gainParam->convertTo0to1(map.yToWeightDb(e.position.y)));
            return;
        }

        if (pinDrag.pin >= 0) {
            auto* p = apvts.getParameter(pinParamID(pinDrag.pin));
            if (p == nullptr) return;
            if (!pinDrag.gestureStarted) {
                workflow.beginUiEdit("Move range boundary");
                workflow.markUiParameter(pinParamID(pinDrag.pin));
                p->beginChangeGesture();
                pinDrag.gestureStarted = true;
            }
            p->setValueNotifyingHost(p->convertTo0to1(map.xToFreq(e.position.x)));
        }
    }

    void mouseUp(const juce::MouseEvent&) override {
        finishGesture();
    }

    void finishGesture() {
        if (dotDrag.band >= 0) {
            if (dotDrag.gestureStarted) {
                if (auto* freqParam = apvts.getParameter(wbandID(dotDrag.band, "Freq")))
                    freqParam->endChangeGesture();
                if (dotDrag.isGain)
                    if (auto* gainParam = apvts.getParameter(wbandID(dotDrag.band, "Gain")))
                        gainParam->endChangeGesture();
                workflow.endUiEdit();
            }
            dotDrag = {};
        }
        if (pinDrag.pin >= 0) {
            if (pinDrag.gestureStarted)
                if (auto* p = apvts.getParameter(pinParamID(pinDrag.pin)))
                    p->endChangeGesture();
            if (pinDrag.gestureStarted) workflow.endUiEdit();
            pinDrag = {};
        }
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override {
        finishGesture();

        const int hitDot = findHandleAt(e.position);
        if (hitDot >= 0) {
            setBoolGestured(wbandID(hitDot, "On"), !snapshot(hitDot).on);
            selectBand(hitDot);
            return;
        }

        if (findPinAt(e.position.x) >= 0) return;  // handled by the click above

        const int band = workflow.firstAbsentBand();
        if (band < 0) return;  // every band slot is present

        const BandSnapshot s = snapshot(band);
        const bool isGain = svfcoeffs::isGainType(static_cast<svfcoeffs::Type>(s.type));
        workflow.createBand(band, map.xToFreq(e.position.x),
                            isGain ? map.yToWeightDb(e.position.y) : 0.0f);
        selectBand(band);
    }

    // Scroll over a band dot adjusts its Q -- mirrors DraggerLayer.h's plain
    // (non-slope) wheel branch exactly; there's no wband Slope param, so the
    // cmd+wheel slope special-case DraggerLayer has doesn't exist here.
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
        const int hit = findHandleAt(e.position);
        if (hit < 0) return;
        const float factor = wheel.deltaY > 0.0f ? 1.1f : (1.0f / 1.1f);
        setFloatGestured(wbandID(hit, "Q"), juce::jlimit(kMinQ, kMaxQ, snapshot(hit).qVal * factor));
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (selectedBand < 0) return false;
        const bool coarse = key.getModifiers().isShiftDown();
        const BandSnapshot s = snapshot(selectedBand);

        if (key.isKeyCode(juce::KeyPress::leftKey) || key.isKeyCode(juce::KeyPress::rightKey)) {
            const float factor = coarse ? 1.05946f : 1.0116f;  // ~1 semitone / ~0.2 semitone
            const float dir = key.isKeyCode(juce::KeyPress::rightKey) ? factor : 1.0f / factor;
            setFloatGestured(wbandID(selectedBand, "Freq"),
                              juce::jlimit(20.0f, 20000.0f, s.freqHz * dir));
            return true;
        }

        if (key.isKeyCode(juce::KeyPress::upKey) || key.isKeyCode(juce::KeyPress::downKey)) {
            // Gain-less types have no meaningful up/down nudge (their Gain
            // param carries no meaning -- see handleCenter's comment), so the
            // key is simply not consumed, unlike DraggerLayer which repurposes
            // this axis for Q on gain-less types.
            if (!svfcoeffs::isGainType(static_cast<svfcoeffs::Type>(s.type))) return false;
            const bool up = key.isKeyCode(juce::KeyPress::upKey);
            const float step = coarse ? 1.0f : 0.1f;
            setFloatGestured(wbandID(selectedBand, "Gain"),
                              juce::jlimit(-24.0f, 24.0f, s.gainDb + (up ? step : -step)));
            return true;
        }

        return false;
    }

private:
    static constexpr int kNumPoints = 400;
    static constexpr int kInset = 6;  // mirrors SpectralAnalyzerView's kInset
    static constexpr float kHitRadius = 10.0f;
    static constexpr float kPinHalfWidth = 3.5f;      // pin visual half-width (~7 px wide pill)
    static constexpr float kPinHeight = 28.0f;        // pin visual height (~2 band-dot diameters)
    static constexpr float kPinHitHalfWidth = 12.0f;  // hit-target half-width (>=24 px wide; height is the whole column, unchanged)
    static constexpr double kFallbackFs = 48000.0;
    // Must match the wbandID(i, "Q") NormalisableRange in PluginProcessor.cpp.
    static constexpr float kMinQ = 0.3f;
    static constexpr float kMaxQ = 12.0f;

    static constexpr std::array<const char*, 5> kBandSuffixes{"On", "Type", "Freq", "Gain", "Q"};
    // Must match kWeightTypeChoices in PluginProcessor.cpp exactly (svfcoeffs::
    // Type order). No slope submenu -- there is no wband Slope param.
    static inline const juce::StringArray kTypeChoices{
        "Bell", "Low Shelf", "High Shelf", "Tilt", "Notch", "Bandpass", "Lowpass", "Highpass"};

    struct BandSnapshot {
        bool on = false;
        int type = 0;
        float freqHz = 1000.0f;
        float gainDb = 0.0f;
        float qVal = 0.7f;
    };

    struct DotDrag {
        int band = -1;
        bool gestureStarted = false;
        bool isGain = false;
    };

    struct PinDrag {
        int pin = -1;  // 0 = rangeLo, 1 = rangeHi
        bool gestureStarted = false;
    };

    // 0-based, mirrors PluginProcessor.cpp's wbandID() exactly ("wband" + i +
    // suffix, i = 0..7) — same duplicated-mechanical-helper convention
    // DraggerLayer.h/CurveView.h use for the EQ's 1-based bandID().
    static juce::String wbandID(int i, const char* suffix) {
        return "wband" + juce::String(i) + suffix;
    }

    static const char* pinParamID(int pin) { return pin == 0 ? "rangeLo" : "rangeHi"; }

    static int parseWbandIndex(const juce::String& paramID) {
        if (!paramID.startsWith("wband")) return -1;
        int i = 5;
        int num = 0;
        bool any = false;
        while (i < paramID.length() && juce::CharacterFunctions::isDigit(paramID[i])) {
            num = num * 10 + (paramID[i] - '0');
            ++i;
            any = true;
        }
        return any ? num : -1;  // already 0-based, no offset
    }

    double readHostFs() const {
        const double fs = hostSampleRate.load(std::memory_order_relaxed);
        return fs > 0.0 ? fs : kFallbackFs;
    }

    float normOf(const juce::String& paramID) const {
        auto* p = apvts.getParameter(paramID);
        auto* raw = apvts.getRawParameterValue(paramID);
        return (p != nullptr && raw != nullptr) ? p->convertTo0to1(raw->load()) : 0.0f;
    }

    BandSnapshot snapshot(int i) const {
        BandSnapshot s;
        s.on = apvts.getRawParameterValue(wbandID(i, "On"))->load() > 0.5f;
        s.type = static_cast<int>(apvts.getRawParameterValue(wbandID(i, "Type"))->load());
        s.freqHz = apvts.getRawParameterValue(wbandID(i, "Freq"))->load();
        s.gainDb = apvts.getRawParameterValue(wbandID(i, "Gain"))->load();
        s.qVal = apvts.getRawParameterValue(wbandID(i, "Q"))->load();
        return s;
    }

    // Gain-less types sit AT THE BASELINE (0 dB), not on the composite curve —
    // their gain param carries no meaning, so pinning them to 0 dB keeps the
    // handle position unambiguous regardless of what the curve happens to do
    // at f0 (e.g. a notch dips well below 0 there).
    juce::Point<float> handleCenter(const BandSnapshot& s) const {
        const float x = map.freqToX(s.freqHz);
        const float yDb = svfcoeffs::isGainType(static_cast<svfcoeffs::Type>(s.type)) ? s.gainDb : 0.0f;
        return {x, map.weightDbToY(yDb)};
    }

    int findHandleAt(juce::Point<float> pos) const {
        int best = -1;
        float bestDist = kHitRadius;
        for (int i = 0; i < kNumWeightBands; ++i) {
            if (!workflow.isBandPresent(i)) continue;
            const BandSnapshot s = snapshot(i);
            const float d = handleCenter(s).getDistanceFrom(pos);
            if (d <= bestDist) {
                bestDist = d;
                best = i;
            }
        }
        return best;
    }

    // Range pins: draggable horizontally only, so hit-testing ignores mouseY
    // entirely (the whole vertical pill is one hit target).
    int findPinAt(float xpx) const {
        const float loHz = apvts.getRawParameterValue("rangeLo")->load();
        const float hiHz = apvts.getRawParameterValue("rangeHi")->load();
        if (std::abs(xpx - map.freqToX(loHz)) <= kPinHitHalfWidth) return 0;
        if (std::abs(xpx - map.freqToX(hiHz)) <= kPinHitHalfWidth) return 1;
        return -1;
    }

    void updateGhost(int hitDot, int hitPin, juce::Point<float> pos) {
        bool newHasGhost = false;
        int newBand = -1;
        juce::Point<float> newCenter;

        if (hitDot < 0 && hitPin < 0) {
            const int band = workflow.firstAbsentBand();
            if (band >= 0) {
                BandSnapshot s = snapshot(band);
                s.freqHz = map.xToFreq(pos.x);
                if (svfcoeffs::isGainType(static_cast<svfcoeffs::Type>(s.type)))
                    s.gainDb = map.yToWeightDb(pos.y);
                else
                    s.gainDb = 0.0f;
                newCenter = handleCenter(s);
                newHasGhost = true;
                newBand = band;
            }
        }

        if (newHasGhost != hasGhost || newBand != ghostBandIdx ||
            (newHasGhost && newCenter != ghostCenter)) {
            hasGhost = newHasGhost;
            ghostBandIdx = newBand;
            ghostCenter = newCenter;
            repaint();
        }
    }

    void selectBand(int b) {
        if (b == selectedBand) return;
        selectedBand = b;
        if (onSelectionChanged) onSelectionChanged(b);
        repaint();
    }

    void setFloatGestured(const juce::String& id, float raw) {
        if (auto* p = apvts.getParameter(id)) {
            workflow.beginUiEdit();
            workflow.markUiParameter(id);
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(raw));
            p->endChangeGesture();
            workflow.endUiEdit();
        }
    }

    void setBoolGestured(const juce::String& id, bool on) {
        setFloatGestured(id, on ? 1.0f : 0.0f);
    }

    void setChoiceGestured(const juce::String& id, int index) {
        setFloatGestured(id, static_cast<float>(index));
    }

    // Right-click menu: type submenu + bypass/delete -- mirrors DraggerLayer.h's
    // showContextMenu() minus the Slope submenu (no wband Slope param).
    void showContextMenu(int band) {
        const BandSnapshot s = snapshot(band);
        juce::PopupMenu menu;
        for (int t = 0; t < kTypeChoices.size(); ++t)
            menu.addItem(100 + t, kTypeChoices[t], true, s.type == t);

        menu.addSeparator();
        menu.addItem(998, s.on ? "Bypass Band" : "Enable Band");
        menu.addItem(999, "Delete Band");

        menu.showMenuAsync(juce::PopupMenu::Options(),
                          [safe = juce::Component::SafePointer<WeightBandLayer>(this), band](int result) {
            if (safe == nullptr) return;
            if (result == 0) return;
            if (result == 999) {
                safe->workflow.deleteBand(band);
                safe->selectBand(-1);
            } else if (result == 998) {
                safe->setBoolGestured(wbandID(band, "On"), !safe->snapshot(band).on);
            } else if (result >= 100) {
                safe->setChoiceGestured(wbandID(band, "Type"), result - 100);
            }
        });
    }

    void drawDot(juce::Graphics& g, int band, juce::Point<float> c, bool selected,
                 bool enabled, float alphaMul = 1.0f) {
        namespace pl = fixxy::palettelight;
        const auto colour = fixxy::palettelight::bandColour(band);
        const float r = selected ? 6.5f : 5.0f;

        if (selected) {
            g.setColour(colour.withAlpha(0.30f));
            g.drawEllipse(c.x - r - 5.0f, c.y - r - 5.0f, (r + 5.0f) * 2.0f, (r + 5.0f) * 2.0f, 2.5f);
        }

        if (enabled) {
            g.setColour(colour.withAlpha((selected ? 1.0f : 0.9f) * alphaMul));
            g.fillEllipse(c.x - r, c.y - r, r * 2.0f, r * 2.0f);
            g.setColour(pl::col(pl::textPrimary).withAlpha(0.55f * alphaMul));
            g.drawEllipse(c.x - r, c.y - r, r * 2.0f, r * 2.0f, 1.0f);
        } else {
            g.setColour(colour.withAlpha((selected ? 1.0f : 0.75f) * alphaMul));
            g.drawEllipse(c.x - r, c.y - r, r * 2.0f, r * 2.0f, 2.0f);
        }
    }

    void drawRangePins(juce::Graphics& g) {
        namespace pl = fixxy::palettelight;
        const float loHz = apvts.getRawParameterValue("rangeLo")->load();
        const float hiHz = apvts.getRawParameterValue("rangeHi")->load();
        drawPin(g, map.freqToX(loHz), hoveredPin == 0 || pinDrag.pin == 0);
        drawPin(g, map.freqToX(hiHz), hoveredPin == 1 || pinDrag.pin == 1);
    }

    void drawPin(juce::Graphics& g, float x, bool emphasised) {
        namespace pl = fixxy::palettelight;

        // Faint hairline through the well as a position cue — owner's
        // complaint was the heavy tall bar, not having a cue at this x.
        g.setColour(pl::col(pl::gridMinor));
        g.drawLine(x, map.y, x, map.y + map.h, 1.0f);

        // Short rounded pill, soothe3-proportioned: about two band-dot
        // diameters tall, vertically centred on the reduction baseline.
        const float halfH = kPinHeight * 0.5f;
        auto r = juce::Rectangle<float>(x - kPinHalfWidth, map.baselineY() - halfH,
                                         kPinHalfWidth * 2.0f, kPinHeight);
        g.setColour((emphasised ? pl::col(pl::textPrimary) : pl::col(pl::axis)).withAlpha(0.5f));
        g.fillRoundedRectangle(r, kPinHalfWidth);
    }

    // Full recompute of the composite curve: gateDb(f, rangeLo, rangeHi) once,
    // plus the dB sum of every enabled band's magnitude at f. Hoists each
    // band's spectralweight::BandWeightEval out of the point loop (it
    // precomputes the SectionRecipe(s) once), then evaluates it at each of
    // the kNumPoints display frequencies -- mirrors CurveView::recomputeBand's
    // hoist, just summed across bands instead of cached per band (8 bands x
    // 400 points is cheap enough at a 30 Hz timer cadence that a per-band
    // cache would be pure ceremony here). BandWeightEval is the SAME
    // evaluator SpectralNode's audio-thread weight table uses -- see this
    // file's header comment.
    void recomputeCurve() {
        const double fs = readHostFs();
        const double rangeLo = static_cast<double>(apvts.getRawParameterValue("rangeLo")->load());
        const double rangeHi = static_cast<double>(apvts.getRawParameterValue("rangeHi")->load());

        std::array<spectralweight::BandWeightEval, kNumWeightBands> evals{};
        std::array<bool, kNumWeightBands> enabled{};
        for (int i = 0; i < kNumWeightBands; ++i) {
            const BandSnapshot s = snapshot(i);
            enabled[static_cast<std::size_t>(i)] = s.on;
            if (s.on) {
                spectralweight::WeightBand wb;
                wb.on = s.on;
                wb.type = s.type;
                wb.freqHz = s.freqHz;
                wb.gainDb = s.gainDb;
                wb.q = s.qVal;
                evals[static_cast<std::size_t>(i)] = spectralweight::BandWeightEval(wb, fs);
            }
        }

        curvePath.clear();
        for (int p = 0; p < kNumPoints; ++p) {
            const float f = freqPoints[static_cast<std::size_t>(p)];
            double sumDb = spectralweight::gateDb(f, rangeLo, rangeHi);
            for (int i = 0; i < kNumWeightBands; ++i)
                if (enabled[static_cast<std::size_t>(i)])
                    sumDb += evals[static_cast<std::size_t>(i)].db(f);

            const float x = map.freqToX(f);
            const float y = map.weightDbToY(static_cast<float>(sumDb));
            if (p == 0) curvePath.startNewSubPath(x, y);
            else curvePath.lineTo(x, y);
        }
    }

    void timerCallback() override {
        const auto presenceMask = workflow.bandPresenceMask();
        if (presenceMask != lastPresenceMask) {
            lastPresenceMask = presenceMask;
            dirty = true;
            if (selectedBand >= 0 && !workflow.isBandPresent(selectedBand))
                selectBand(-1);
        }
        if (dirty.exchange(false)) {
            recomputeCurve();
            repaint();
        }
    }

    // Real-time-safe-ish: this can fire from whatever thread the host
    // automates a parameter on (see DraggerLayer's identical comment).
    void parameterChanged(const juce::String& paramID, float newValue) override {
        if (paramID == "rangeLo" || paramID == "rangeHi") {
            auto* p = apvts.getParameter(paramID);
            const float norm = p != nullptr ? p->convertTo0to1(newValue) : 0.0f;
            float& cache = (paramID == "rangeLo") ? lastRangeLoNorm : lastRangeHiNorm;
            if (std::abs(norm - cache) < 0.001f) return;
            cache = norm;
            dirty = true;
            return;
        }

        const int idx = parseWbandIndex(paramID);
        if (idx < 0) return;
        const auto ci = static_cast<std::size_t>(idx);

        std::array<float, kNumWeightBands>* bandCache = nullptr;
        if (paramID.endsWith("Freq")) bandCache = &lastFreqNorm;
        else if (paramID.endsWith("Gain")) bandCache = &lastGainNorm;
        else if (paramID.endsWith("Q")) bandCache = &lastQNorm;

        if (bandCache != nullptr) {
            auto* p = apvts.getParameter(paramID);
            const float norm = p != nullptr ? p->convertTo0to1(newValue) : 0.0f;
            // Feedback-echo guard: skip the recompute if this is just the host
            // reflecting back (near-)exactly the value we ourselves wrote a
            // moment ago via setValueNotifyingHost.
            if (std::abs(norm - (*bandCache)[ci]) < 0.001f) return;
            (*bandCache)[ci] = norm;
        }
        // On/Type: discrete, always dirty.

        dirty = true;
    }

    juce::AudioProcessorValueTreeState& apvts;
    const std::atomic<double>& hostSampleRate;
    SpectralWorkflow& workflow;
    std::uint32_t lastPresenceMask = 0u;
    spectralui::DisplayMap map;

    int selectedBand = -1;
    DotDrag dotDrag;
    PinDrag pinDrag;
    int hoveredPin = -1;

    bool hasGhost = false;
    int ghostBandIdx = -1;
    juce::Point<float> ghostCenter;

    std::array<float, kNumPoints> freqPoints{};
    juce::Path curvePath;
    std::atomic<bool> dirty{false};

    std::array<float, kNumWeightBands> lastFreqNorm{};
    std::array<float, kNumWeightBands> lastGainNorm{};
    std::array<float, kNumWeightBands> lastQNorm{};
    float lastRangeLoNorm = 0.0f;
    float lastRangeHiNorm = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WeightBandLayer)
};
