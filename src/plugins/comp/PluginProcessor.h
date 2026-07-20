#pragma once

#include <atomic>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "dsp/CompressorNode.h"

/**
    LpngComp — feed-forward log-domain compressor product. Thin plugin shell over
    the JUCE-free CompressorNode (src/dsp/): APVTS params -> once-per-block smoothed
    Params -> node. GenericAudioProcessorEditor for v1 (custom UI later).

    NO lookahead in v1 -> latency = 0, nothing reported to the host.
*/
class CompProcessor : public juce::AudioProcessor {
public:
    CompProcessor();

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Current gain-reduction magnitude (dB, >= 0), published each block for a
    // future meter. Atomic so the message-thread reader needs no lock.
    std::atomic<float> gainReductionDb{0.0f};

    // Input/output peak level (dBFS, block max |sample| across all channels),
    // published each block for future input/output meters. -100 dB is the
    // floor used for silent (zero) blocks, since dB(0) is -inf. Same
    // cross-thread publish pattern as gainReductionDb.
    std::atomic<float> inputPeakDb{-100.0f};
    std::atomic<float> outputPeakDb{-100.0f};

    // Block-max detector level x_L (dB, post SC-HPF/RMS conditioning) — where
    // the signal sits on the static curve's input axis. Feeds the editor's live
    // IO dot at (x_L, staticCurve(x_L)). -120 dB is the detector's own log
    // floor. Same publish pattern as gainReductionDb.
    std::atomic<float> scLevelDb{-120.0f};

    // Sample rate mirrored from prepareToPlay for the UI thread (trace
    // displays later) — mirrors the EQ plugin's publishedSampleRate pattern.
    std::atomic<double> publishedSampleRate{0.0};

    float getGainReductionDb() const { return gainReductionDb.load(std::memory_order_relaxed); }
    float getScLevelDb() const { return scLevelDb.load(std::memory_order_relaxed); }
    float getInputPeakDb() const { return inputPeakDb.load(std::memory_order_relaxed); }
    float getOutputPeakDb() const { return outputPeakDb.load(std::memory_order_relaxed); }
    double getPublishedSampleRate() const {
        return publishedSampleRate.load(std::memory_order_relaxed);
    }

private:
    // Continuous params smoothed at the plugin boundary (mirrors the EQ pattern)
    // so the DSP node stays a pure JUCE-free kernel. The param->coeff map (exp,
    // pow) is nonlinear, so a linear ramp on the dB/ms quantities keeps the
    // perceptual values moving smoothly; coeffs recompute once per block.
    std::atomic<float>* thresholdParam = nullptr;
    std::atomic<float>* ratioParam = nullptr;
    std::atomic<float>* kneeParam = nullptr;
    std::atomic<float>* attackParam = nullptr;
    std::atomic<float>* releaseParam = nullptr;
    std::atomic<float>* makeupParam = nullptr;
    // MIX and RANGE are smoothed: MIX is a direct dry/wet gain blend and RANGE
    // clamps the applied gain directly, so a per-block step in either would click.
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* rangeParam = nullptr;
    // Discrete params read directly (no smoothing). HOLD only gates an integer
    // sample counter's threshold — it changes no gain value continuously, so a
    // per-block step is inaudible and it needs no ramp.
    std::atomic<float>* holdParam = nullptr;
    std::atomic<float>* autoMakeupParam = nullptr;
    std::atomic<float>* progRelParam = nullptr;
    std::atomic<float>* scHpfParam = nullptr;
    std::atomic<float>* rmsParam = nullptr;

    juce::LinearSmoothedValue<double> thresholdSmoother;
    juce::LinearSmoothedValue<double> ratioSmoother;
    juce::LinearSmoothedValue<double> kneeSmoother;
    juce::LinearSmoothedValue<double> attackSmoother;
    juce::LinearSmoothedValue<double> releaseSmoother;
    juce::LinearSmoothedValue<double> makeupSmoother;
    juce::LinearSmoothedValue<double> mixSmoother;
    juce::LinearSmoothedValue<double> rangeSmoother;

    CompressorNode comp;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CompProcessor)
};
