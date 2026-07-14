#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "dsp/SvfPeakNode.h"

class MetaPluginProcessor : public juce::AudioProcessor {
public:
    MetaPluginProcessor();

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

private:
    std::atomic<float>* gainParamDb = nullptr;
    juce::LinearSmoothedValue<float> gainSmoother;

    // --- Peaking EQ band (TPT-SVF node) ---
    // Smoothing lives at the plugin boundary (mirrors gainSmoother) so the node
    // stays a pure JUCE-free DSP kernel. We smooth the three *raw* params, not
    // the derived coefficients: the mapping param->coeff (tan, 10^, division)
    // is nonlinear, so a linear ramp on the params keeps the perceptually
    // meaningful quantities (Hz, dB) moving smoothly, and the SVF is stable
    // under per-block coefficient updates by construction. Coeffs are then
    // recomputed once per block from the smoothed values — never per sample.
    std::atomic<float>* eqFreqParam = nullptr;
    std::atomic<float>* eqQParam = nullptr;
    std::atomic<float>* eqGainParam = nullptr;
    juce::LinearSmoothedValue<double> eqFreqSmoother;
    juce::LinearSmoothedValue<double> eqQSmoother;
    juce::LinearSmoothedValue<double> eqGainSmoother;
    SvfPeakNode eqBand;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetaPluginProcessor)
};
