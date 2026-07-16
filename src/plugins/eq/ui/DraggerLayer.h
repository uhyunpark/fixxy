#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "CoordMap.h"
#include "CurveView.h"
#include "dsp/MatchedCoeffs.h"

/**
    Stage 3: the interactive layer. Topmost component in the curve stack,
    owns all mouse/keyboard interaction with the 16 bands. Nothing here
    touches the audio thread — every edit goes through APVTS's normal
    message-thread gesture protocol (beginChangeGesture / setValueNotifyingHost
    / endChangeGesture), exactly like a host-drawn slider would.

    Handle position is never cached: paint() (and mouse hit-testing) always
    reads the CURRENT apvts raw values and recomputes x/y on the spot. For
    gain-bearing types that's one dB lookup; for gain-less types (notch/BP/
    LP/HP) it's one svfcoeffs::magnitudeDb() eval at f0 -- cheap, and it keeps
    the handle glued exactly to the curve CurveView is drawing from the same
    ground truth.

    A parameter listener (same dirty-flag/timer pattern as CurveView) exists
    only to know WHEN to repaint -- e.g. host automation moving a band while
    the editor is open. It applies a normalized-delta guard (0.001) on the
    three continuous params (Freq/Gain/Q) so that reading back the exact
    value we just wrote during a drag doesn't spam extra repaints.
*/
class DraggerLayer : public juce::Component,
                      private juce::Timer,
                      private juce::AudioProcessorValueTreeState::Listener {
public:
    static constexpr int kNumBands = CurveView::kNumBands;

    /** Fired whenever a click/double-click changes which band is selected
        (never fired for the external setSelectedBandExternal() path, so the
        band-select combo <-> DraggerLayer sync in EqEditor can't loop). */
    std::function<void(int)> onSelectionChanged;

    DraggerLayer(juce::AudioProcessorValueTreeState& state, CoordMap& mapIn, CurveView& curveViewIn,
                 const std::atomic<double>& hostFs)
        : apvts(state), map(mapIn), curveView(curveViewIn), hostSampleRate(hostFs) {
        setWantsKeyboardFocus(true);

        for (int i = 0; i < kNumBands; ++i)
            for (const char* suffix : kSuffixes)
                apvts.addParameterListener(bandID(i, suffix), this);

        for (int i = 0; i < kNumBands; ++i) {
            const auto ci = static_cast<std::size_t>(i);
            lastFreqNorm[ci] = normOf(bandID(i, "Freq"));
            lastGainNorm[ci] = normOf(bandID(i, "Gain"));
            lastQNorm[ci] = normOf(bandID(i, "Q"));
        }

        startTimerHz(30);
    }

    ~DraggerLayer() override {
        for (int i = 0; i < kNumBands; ++i)
            for (const char* suffix : kSuffixes)
                apvts.removeParameterListener(bandID(i, suffix), this);
    }

    /** Called from EqEditor when the band-select combo changes directly --
        keeps this layer's selection (and CurveView's emphasis) in sync
        without re-firing onSelectionChanged back at the combo. */
    void setSelectedBandExternal(int band) {
        if (selectedBand == band) return;
        selectedBand = band;
        curveView.setSelectedBand(band);
        repaint();
    }

    void paint(juce::Graphics& g) override {
        const double fs = readHostFs();
        for (int i = 0; i < kNumBands; ++i) {
            const BandSnapshot s = snapshot(i);
            if (!s.enabled) continue;

            const auto c = handleCenter(s, fs);
            const bool selected = (i == selectedBand);
            const float r = selected ? 7.0f : 5.0f;

            g.setColour(bandColour(i).withAlpha(selected ? 1.0f : 0.85f));
            g.fillEllipse(c.x - r, c.y - r, r * 2.0f, r * 2.0f);
            g.setColour(juce::Colours::black.withAlpha(0.6f));
            g.drawEllipse(c.x - r, c.y - r, r * 2.0f, r * 2.0f, 1.0f);

            if (selected) {
                g.setColour(juce::Colours::white);
                g.setFont(juce::FontOptions(10.0f));
                g.drawText("B" + juce::String(i + 1), static_cast<int>(c.x) - 15,
                           static_cast<int>(c.y) - 22, 30, 14, juce::Justification::centred);
            }
        }
    }

    void mouseMove(const juce::MouseEvent& e) override {
        const int hit = findHandleAt(e.position);
        setMouseCursor(hit >= 0 ? juce::MouseCursor::PointingHandCursor
                                 : juce::MouseCursor::NormalCursor);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        grabKeyboardFocus();
        const int hit = findHandleAt(e.position);

        if (e.mods.isPopupMenu()) {
            if (hit >= 0) {
                selectBand(hit);
                showContextMenu(hit);
            }
            return;
        }

        if (hit < 0) {
            selectBand(-1);
            return;
        }

        selectBand(hit);
        const BandSnapshot s = snapshot(hit);
        drag.band = hit;
        drag.gestureStarted = false;
        drag.isGain = isGainType(s.type);
        drag.startMouseY = e.position.y;
        drag.startQ = s.qVal;
    }

    // Lazily starts the APVTS gesture on the first actual movement, not on
    // mouseDown -- a plain click (select, no movement) never opens a gesture
    // bracket, and a double-click's second mouseDown doesn't nudge the band
    // before mouseDoubleClick gets to run.
    void mouseDrag(const juce::MouseEvent& e) override {
        if (drag.band < 0) return;
        auto* freqParam = apvts.getParameter(bandID(drag.band, "Freq"));
        auto* secondaryParam = apvts.getParameter(bandID(drag.band, drag.isGain ? "Gain" : "Q"));
        if (freqParam == nullptr || secondaryParam == nullptr) return;

        if (!drag.gestureStarted) {
            freqParam->beginChangeGesture();
            secondaryParam->beginChangeGesture();
            drag.gestureStarted = true;
        }

        const float freqHz = map.xToFreq(e.position.x);
        freqParam->setValueNotifyingHost(freqParam->convertTo0to1(freqHz));

        if (drag.isGain) {
            const float gainDb = map.yToDb(e.position.y);
            secondaryParam->setValueNotifyingHost(secondaryParam->convertTo0to1(gainDb));
        } else {
            // Gain-less types: vertical drag has no direct visual meaning (the
            // handle already sits ON the curve), so it's a relative,
            // exponential Q control instead. Up = higher Q.
            const float deltaPx = drag.startMouseY - e.position.y;
            const float q = juce::jlimit(kMinQ, kMaxQ,
                                          drag.startQ * std::pow(2.0f, deltaPx / kQPixelsPerOctave));
            secondaryParam->setValueNotifyingHost(secondaryParam->convertTo0to1(q));
        }
    }

    void mouseUp(const juce::MouseEvent&) override {
        if (drag.band < 0) return;
        if (drag.gestureStarted) {
            if (auto* freqParam = apvts.getParameter(bandID(drag.band, "Freq")))
                freqParam->endChangeGesture();
            if (auto* secondaryParam =
                    apvts.getParameter(bandID(drag.band, drag.isGain ? "Gain" : "Q")))
                secondaryParam->endChangeGesture();
        }
        drag = {};
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override {
        drag = {};  // cancel any drag state the 2nd mouseDown of this click left behind

        const int hit = findHandleAt(e.position);
        if (hit >= 0) {
            setEnabledGestured(hit, false);
            return;
        }

        const int nearest = findNearestDisabledBand(e.position.x);
        if (nearest < 0) return;  // every band already enabled

        setFloatGestured(bandID(nearest, "Freq"), map.xToFreq(e.position.x));
        const BandSnapshot s = snapshot(nearest);
        if (isGainType(s.type))
            setFloatGestured(bandID(nearest, "Gain"), map.yToDb(e.position.y));
        setEnabledGestured(nearest, true);
        selectBand(nearest);
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
        const int hit = findHandleAt(e.position);
        if (hit < 0) return;
        const BandSnapshot s = snapshot(hit);

        if (e.mods.isCommandDown() && (s.type == kLowpass || s.type == kHighpass)) {
            const int dir = wheel.deltaY > 0.0f ? 1 : -1;
            const int newIdx = juce::jlimit(0, kNumSlopes - 1, s.slopeIdx + dir);
            if (newIdx != s.slopeIdx) setChoiceGestured(bandID(hit, "Slope"), newIdx);
            return;
        }

        const float factor = wheel.deltaY > 0.0f ? 1.1f : (1.0f / 1.1f);
        setFloatGestured(bandID(hit, "Q"), juce::jlimit(kMinQ, kMaxQ, s.qVal * factor));
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (selectedBand < 0) return false;
        const bool coarse = key.getModifiers().isShiftDown();
        const BandSnapshot s = snapshot(selectedBand);

        if (key.isKeyCode(juce::KeyPress::leftKey) || key.isKeyCode(juce::KeyPress::rightKey)) {
            const float factor = coarse ? 1.05946f : 1.0116f;  // ~1 semitone / ~0.2 semitone
            const float dir = key.isKeyCode(juce::KeyPress::rightKey) ? factor : 1.0f / factor;
            setFloatGestured(bandID(selectedBand, "Freq"),
                              juce::jlimit(20.0f, 20000.0f, s.freqHz * dir));
            return true;
        }

        if (key.isKeyCode(juce::KeyPress::upKey) || key.isKeyCode(juce::KeyPress::downKey)) {
            const bool up = key.isKeyCode(juce::KeyPress::upKey);
            if (isGainType(s.type)) {
                const float step = coarse ? 1.0f : 0.1f;
                setFloatGestured(bandID(selectedBand, "Gain"),
                                  juce::jlimit(-24.0f, 24.0f, s.gainDb + (up ? step : -step)));
            } else {
                const float factor = coarse ? 1.1f : 1.02f;
                setFloatGestured(bandID(selectedBand, "Q"),
                                  juce::jlimit(kMinQ, kMaxQ, s.qVal * (up ? factor : 1.0f / factor)));
            }
            return true;
        }

        return false;
    }

private:
    static constexpr std::array<const char*, 6> kSuffixes{
        "Enabled", "Type", "Freq", "Q", "Gain", "Slope"};
    static constexpr int kLowpass = 6;   // svfcoeffs::Type::Lowpass
    static constexpr int kHighpass = 7;  // svfcoeffs::Type::Highpass
    static constexpr int kNumSlopes = 9;
    static constexpr float kMinQ = 0.1f;
    static constexpr float kMaxQ = 18.0f;
    static constexpr float kHitRadius = 10.0f;
    static constexpr float kQPixelsPerOctave = 100.0f;
    static constexpr double kFallbackFs = 48000.0;
    // Must match kSlopeValues in PluginProcessor.cpp (choice index -> dB/oct).
    static constexpr std::array<double, 9> kSlopeValues{6, 12, 18, 24, 30, 36, 48, 72, 96};
    // Must match the choice order in PluginProcessor.cpp exactly.
    static inline const juce::StringArray kTypeChoices{
        "Bell", "Low Shelf", "High Shelf", "Tilt", "Notch", "Bandpass", "Lowpass", "Highpass"};
    static inline const juce::StringArray kSlopeChoices{
        "6", "12", "18", "24", "30", "36", "48", "72", "96"};

    struct BandSnapshot {
        bool enabled = false;
        int type = 0;
        int slopeIdx = 0;
        float freqHz = 1000.0f;
        float qVal = 0.7071f;
        float gainDb = 0.0f;
    };

    struct DragState {
        int band = -1;
        bool gestureStarted = false;
        bool isGain = false;
        float startMouseY = 0.0f;
        float startQ = 0.7071f;
    };

    static juce::String bandID(int i, const char* suffix) {
        return "band" + juce::String(i + 1) + suffix;
    }

    static bool isGainType(int type) { return type >= 0 && type <= 3; }  // Bell/LowShelf/HighShelf/Tilt

    static juce::Colour bandColour(int i) {
        return juce::Colour::fromHSV(static_cast<float>(i) / static_cast<float>(kNumBands), 0.65f,
                                      0.95f, 1.0f);
    }

    static int parseBandIndex(const juce::String& paramID) {
        if (!paramID.startsWith("band")) return -1;
        int i = 4;
        int num = 0;
        bool any = false;
        while (i < paramID.length() && juce::CharacterFunctions::isDigit(paramID[i])) {
            num = num * 10 + (paramID[i] - '0');
            ++i;
            any = true;
        }
        return any ? (num - 1) : -1;
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
        s.enabled = apvts.getRawParameterValue(bandID(i, "Enabled"))->load() > 0.5f;
        s.type = static_cast<int>(apvts.getRawParameterValue(bandID(i, "Type"))->load());
        s.slopeIdx = juce::jlimit(
            0, kNumSlopes - 1, static_cast<int>(apvts.getRawParameterValue(bandID(i, "Slope"))->load()));
        s.freqHz = apvts.getRawParameterValue(bandID(i, "Freq"))->load();
        s.qVal = apvts.getRawParameterValue(bandID(i, "Q"))->load();
        s.gainDb = apvts.getRawParameterValue(bandID(i, "Gain"))->load();
        return s;
    }

    juce::Point<float> handleCenter(const BandSnapshot& s, double fs) const {
        const float x = map.freqToX(s.freqHz);
        float yDb;
        if (isGainType(s.type)) {
            yDb = s.gainDb;
        } else {
            const double slope = kSlopeValues[static_cast<std::size_t>(s.slopeIdx)];
            yDb = static_cast<float>(matched::bandMagnitudeDb(static_cast<svfcoeffs::Type>(s.type),
                                                                s.freqHz, s.qVal, s.gainDb, slope, fs,
                                                                s.freqHz));
        }
        return {x, map.dbToY(yDb)};
    }

    int findHandleAt(juce::Point<float> pos) const {
        const double fs = readHostFs();
        int best = -1;
        float bestDist = kHitRadius;
        for (int i = 0; i < kNumBands; ++i) {
            const BandSnapshot s = snapshot(i);
            if (!s.enabled) continue;
            const float d = handleCenter(s, fs).getDistanceFrom(pos);
            if (d <= bestDist) {
                bestDist = d;
                best = i;
            }
        }
        return best;
    }

    int findNearestDisabledBand(float clickX) const {
        const float clickFreq = map.xToFreq(clickX);
        int best = -1;
        float bestDist = std::numeric_limits<float>::max();
        for (int i = 0; i < kNumBands; ++i) {
            const BandSnapshot s = snapshot(i);
            if (s.enabled) continue;
            const float dist = std::abs(std::log10(s.freqHz) - std::log10(clickFreq));
            if (dist < bestDist) {
                bestDist = dist;
                best = i;
            }
        }
        return best;
    }

    void selectBand(int b) {
        if (b == selectedBand) return;
        selectedBand = b;
        curveView.setSelectedBand(b);
        if (onSelectionChanged) onSelectionChanged(b);
        repaint();
    }

    void setFloatGestured(const juce::String& id, float raw) {
        if (auto* p = apvts.getParameter(id)) {
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(raw));
            p->endChangeGesture();
        }
    }

    void setEnabledGestured(int band, bool en) {
        if (auto* p = apvts.getParameter(bandID(band, "Enabled"))) {
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(en ? 1.0f : 0.0f));
            p->endChangeGesture();
        }
    }

    void setChoiceGestured(const juce::String& id, int index) {
        if (auto* p = apvts.getParameter(id)) {
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(index)));
            p->endChangeGesture();
        }
    }

    void showContextMenu(int band) {
        const BandSnapshot s = snapshot(band);
        juce::PopupMenu menu;
        for (int t = 0; t < kTypeChoices.size(); ++t)
            menu.addItem(100 + t, kTypeChoices[t], true, s.type == t);

        if (s.type == kLowpass || s.type == kHighpass) {
            juce::PopupMenu slopeMenu;
            for (int i = 0; i < kSlopeChoices.size(); ++i)
                slopeMenu.addItem(200 + i, kSlopeChoices[i] + " dB/oct", true, s.slopeIdx == i);
            menu.addSubMenu("Slope", slopeMenu);
        }

        menu.addSeparator();
        menu.addItem(999, "Disable Band");

        menu.showMenuAsync(juce::PopupMenu::Options(), [this, band](int result) {
            if (result == 0) return;
            if (result == 999) {
                setEnabledGestured(band, false);
            } else if (result >= 200) {
                setChoiceGestured(bandID(band, "Slope"), result - 200);
            } else if (result >= 100) {
                setChoiceGestured(bandID(band, "Type"), result - 100);
            }
        });
    }

    void timerCallback() override {
        bool any = false;
        for (int i = 0; i < kNumBands; ++i)
            if (dirty[static_cast<std::size_t>(i)].exchange(false)) any = true;
        if (any) repaint();
    }

    // Real-time-safe-ish: this can fire from whatever thread the host
    // automates a parameter on (see CurveView's identical comment). newValue
    // here is the ACTUAL (denormalised) parameter value.
    void parameterChanged(const juce::String& paramID, float newValue) override {
        const int idx = parseBandIndex(paramID);
        if (idx < 0) return;
        const auto ci = static_cast<std::size_t>(idx);

        std::array<float, kNumBands>* cache = nullptr;
        if (paramID.endsWith("Freq")) cache = &lastFreqNorm;
        else if (paramID.endsWith("Gain")) cache = &lastGainNorm;
        else if (paramID.endsWith("Q")) cache = &lastQNorm;

        if (cache != nullptr) {
            auto* p = apvts.getParameter(paramID);
            const float norm = p != nullptr ? p->convertTo0to1(newValue) : 0.0f;
            // Feedback-echo guard: skip the repaint if this is just the host
            // reflecting back (near-)exactly the value we ourselves wrote a
            // moment ago via setValueNotifyingHost.
            if (std::abs(norm - (*cache)[ci]) < 0.001f) return;
            (*cache)[ci] = norm;
        }

        dirty[ci] = true;
    }

    juce::AudioProcessorValueTreeState& apvts;
    CoordMap& map;
    CurveView& curveView;
    const std::atomic<double>& hostSampleRate;

    int selectedBand = -1;
    DragState drag;

    std::array<float, kNumBands> lastFreqNorm{};
    std::array<float, kNumBands> lastGainNorm{};
    std::array<float, kNumBands> lastQNorm{};
    std::array<std::atomic<bool>, kNumBands> dirty{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DraggerLayer)
};
