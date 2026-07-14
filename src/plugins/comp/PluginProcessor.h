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
