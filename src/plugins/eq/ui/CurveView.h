#pragma once

#include <array>
#include <atomic>
#include <cmath>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "CoordMap.h"
#include "dsp/MatchedCoeffs.h"
#include "dsp/MultibandEq.h"
#include "dsp/SvfBandNode.h"

/**
    Response-curve view: per-band dim paths (enabled bands only) + one bright
    sum path. Curve MATH runs off paint() entirely:

      APVTS parameter change -> parameterChanged() sets an atomic per-band
      dirty flag (real-time safe: this can fire from whatever thread the host
      automates a parameter on) -> a 30 Hz juce::Timer drains dirty flags on
      the message thread, recomputing ONLY the bands that changed, using
      svfcoeffs::magnitudeDb() (the same analytic recipe SvfBandNode's kernel
      is built from — see BandResponse.h) -> results are cached as dB arrays
      and pre-stroked juce::Path objects.

      paint() only calls g.strokePath() on those prebuilt paths — no math.

    Sum path = element-wise dB SUM of enabled bands. That's not an
    approximation: MultibandEq runs its bands in series, so the cascade's true
    magnitude response is the PRODUCT of each band's linear magnitude, i.e.
    the SUM of each band's dB magnitude.
*/
class CurveView : public juce::Component,
                   private juce::Timer,
                   private juce::AudioProcessorValueTreeState::Listener {
public:
    static constexpr int kNumBands = MultibandEq::kNumBands;
    static constexpr int kNumPoints = 400;

    /** hostFs: the processor's published sample rate (0.0 until prepared).
        The curve MUST be evaluated at the actual host fs — the bilinear
        response near Nyquist depends on it (several dB of cramping for HF
        bands at 44.1k), so a fixed display rate would draw a wrong curve
        above ~10 kHz in a 44.1 kHz session. Polled in the 30 Hz timer (plain
        atomic read, no locks against the audio thread); a change marks every
        band dirty for recompute. */
    CurveView(juce::AudioProcessorValueTreeState& state, CoordMap& mapIn,
              const std::atomic<double>& hostFs)
        : apvts(state), map(mapIn), hostSampleRate(hostFs) {
        displayFs = readHostFs();
        const float logMin = std::log10(CoordMap::kMinFreq);
        const float logMax = std::log10(CoordMap::kMaxFreq);
        for (int p = 0; p < kNumPoints; ++p) {
            const float t = static_cast<float>(p) / static_cast<float>(kNumPoints - 1);
            freqPoints[static_cast<std::size_t>(p)] =
                std::pow(10.0f, logMin + t * (logMax - logMin));
        }

        for (int i = 0; i < kNumBands; ++i)
            for (const char* suffix : kSuffixes)
                apvts.addParameterListener(bandID(i, suffix), this);

        // Compute every band before the first paint so nothing flashes blank.
        for (int i = 0; i < kNumBands; ++i)
            recomputeBand(i);
        rebuildSumPath();

        startTimerHz(30);
    }

    ~CurveView() override {
        for (int i = 0; i < kNumBands; ++i)
            for (const char* suffix : kSuffixes)
                apvts.removeParameterListener(bandID(i, suffix), this);
    }

    void resized() override {
        // Pixel mapping changed; re-stroke from already-cached dB data (no
        // DSP recompute needed).
        for (int i = 0; i < kNumBands; ++i)
            buildPathForBand(i);
        rebuildSumPath();
        repaint();
    }

    void paint(juce::Graphics& g) override {
        for (int i = 0; i < kNumBands; ++i) {
            if (!enabledCache[static_cast<std::size_t>(i)]) continue;
            const bool selected = (i == selectedBand);
            g.setColour(bandColour(i).withAlpha(selected ? 0.9f : 0.4f));
            g.strokePath(bandPaths[static_cast<std::size_t>(i)],
                         juce::PathStrokeType(selected ? 2.4f : 1.2f));
        }
        g.setColour(juce::Colours::white);
        g.strokePath(sumPath, juce::PathStrokeType(2.2f));
    }

    /** Stage 3 hook: DraggerLayer (and the band-select combo, via EqEditor)
        drive this so the curve emphasizes whichever band is currently
        selected. -1 = no emphasis. Message-thread only, like everything else
        here except the dirty-flag listener callback. */
    void setSelectedBand(int band) {
        if (selectedBand == band) return;
        selectedBand = band;
        repaint();
    }

private:
    static constexpr std::array<const char*, 6> kSuffixes{
        "Enabled", "Type", "Freq", "Q", "Gain", "Slope"};

    static juce::String bandID(int i, const char* suffix) {
        return "band" + juce::String(i + 1) + suffix;
    }

    static juce::Colour bandColour(int i) {
        return juce::Colour::fromHSV(static_cast<float>(i) / static_cast<float>(kNumBands), 0.65f,
                                      0.95f, 1.0f);
    }

    double readHostFs() const {
        const double fs = hostSampleRate.load(std::memory_order_relaxed);
        return fs > 0.0 ? fs : kFallbackFs;  // 0.0 = processor not prepared yet
    }

    void timerCallback() override {
        // Host sample rate change (first prepare, or a session-rate switch)
        // invalidates every cached curve — the bilinear response near Nyquist
        // depends on fs.
        const double fs = readHostFs();
        if (fs != displayFs) {
            displayFs = fs;
            for (auto& d : dirty)
                d = true;
        }

        bool any = false;
        for (int i = 0; i < kNumBands; ++i) {
            if (dirty[static_cast<std::size_t>(i)].exchange(false)) {
                recomputeBand(i);
                any = true;
            }
        }
        if (any) {
            rebuildSumPath();
            repaint();
        }
    }

    void parameterChanged(const juce::String& paramID, float) override {
        const int idx = parseBandIndex(paramID);
        if (idx >= 0)
            dirty[static_cast<std::size_t>(idx)] = true;
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

    void recomputeBand(int i) {
        const auto idx = static_cast<std::size_t>(i);
        const bool enabled = apvts.getRawParameterValue(bandID(i, "Enabled"))->load() > 0.5f;
        const auto type =
            static_cast<SvfBandNode::Type>(static_cast<int>(apvts.getRawParameterValue(bandID(i, "Type"))->load()));
        const double f0 = static_cast<double>(apvts.getRawParameterValue(bandID(i, "Freq"))->load());
        const double q = static_cast<double>(apvts.getRawParameterValue(bandID(i, "Q"))->load());
        const double gainDb = static_cast<double>(apvts.getRawParameterValue(bandID(i, "Gain"))->load());
        const int slopeIdx = static_cast<int>(apvts.getRawParameterValue(bandID(i, "Slope"))->load());
        const double slope = kSlopeValues[static_cast<std::size_t>(
            juce::jlimit(0, static_cast<int>(kSlopeValues.size()) - 1, slopeIdx))];

        enabledCache[idx] = enabled;
        for (int p = 0; p < kNumPoints; ++p) {
            dbCache[idx][static_cast<std::size_t>(p)] = static_cast<float>(matched::bandMagnitudeDb(
                type, f0, q, gainDb, slope, displayFs, freqPoints[static_cast<std::size_t>(p)]));
        }
        buildPathForBand(i);
    }

    void buildPathForBand(int i) {
        const auto idx = static_cast<std::size_t>(i);
        juce::Path& path = bandPaths[idx];
        path.clear();
        for (int p = 0; p < kNumPoints; ++p) {
            const float x = map.freqToX(freqPoints[static_cast<std::size_t>(p)]);
            const float y = map.dbToY(dbCache[idx][static_cast<std::size_t>(p)]);
            if (p == 0) path.startNewSubPath(x, y);
            else path.lineTo(x, y);
        }
    }

    void rebuildSumPath() {
        sumPath.clear();
        for (int p = 0; p < kNumPoints; ++p) {
            float sumDb = 0.0f;
            for (int i = 0; i < kNumBands; ++i)
                if (enabledCache[static_cast<std::size_t>(i)])
                    sumDb += dbCache[static_cast<std::size_t>(i)][static_cast<std::size_t>(p)];

            const float x = map.freqToX(freqPoints[static_cast<std::size_t>(p)]);
            const float y = map.dbToY(sumDb);
            if (p == 0) sumPath.startNewSubPath(x, y);
            else sumPath.lineTo(x, y);
        }
    }

    // Must match kSlopeValues in PluginProcessor.cpp (choice index -> dB/oct).
    static constexpr std::array<double, 9> kSlopeValues{6, 12, 18, 24, 30, 36, 48, 72, 96};
    // Used only before the processor's first prepareToPlay has published a
    // real host rate.
    static constexpr double kFallbackFs = 48000.0;

    juce::AudioProcessorValueTreeState& apvts;
    CoordMap& map;
    const std::atomic<double>& hostSampleRate;
    double displayFs = kFallbackFs;

    std::array<float, kNumPoints> freqPoints{};
    std::array<std::array<float, kNumPoints>, kNumBands> dbCache{};
    std::array<bool, kNumBands> enabledCache{};
    std::array<juce::Path, kNumBands> bandPaths{};
    juce::Path sumPath;
    int selectedBand = -1;

    std::array<std::atomic<bool>, kNumBands> dirty{};
};
