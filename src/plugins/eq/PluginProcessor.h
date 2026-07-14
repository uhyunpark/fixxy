#pragma once

#include <array>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "dsp/MultibandEq.h"

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

    static constexpr int kNumBands = MultibandEq::kNumBands;

private:
    // Resolve the current BandParams for band i from its smoothed/atomic values.
    SvfBandNode::BandParams readBandParams(int i, int numSamples);

    // --- Output gain (unchanged: the old "gain" param is now output trim) ---
    std::atomic<float>* gainParamDb = nullptr;
    juce::LinearSmoothedValue<float> gainSmoother;

    // --- 16 EQ bands ---
    // Smoothing lives at the plugin boundary (mirrors gainSmoother) so the DSP
    // nodes stay pure JUCE-free kernels. We smooth the three CONTINUOUS params
    // (f0/Q/gain) per band — the param->coeff map (tan, 10^, division) is
    // nonlinear, so a linear ramp on Hz/dB keeps the perceptual quantities
    // moving smoothly and the SVF is stable under per-block coeff updates.
    // type/slope/enabled are discrete: enabled routes through MultibandEq's 10 ms
    // crossfade; type/slope changes on an ENABLED band apply immediately and may
    // click (accepted for now per spec — the common path, enabling a band with a
    // chosen type, goes through the fade because enabling resets + fades in).
    std::array<std::atomic<float>*, kNumBands> bandEnabled{};
    std::array<std::atomic<float>*, kNumBands> bandType{};
    std::array<std::atomic<float>*, kNumBands> bandFreq{};
    std::array<std::atomic<float>*, kNumBands> bandQ{};
    std::array<std::atomic<float>*, kNumBands> bandGain{};
    std::array<std::atomic<float>*, kNumBands> bandSlope{};

    std::array<juce::LinearSmoothedValue<double>, kNumBands> freqSmoother{};
    std::array<juce::LinearSmoothedValue<double>, kNumBands> qSmoother{};
    std::array<juce::LinearSmoothedValue<double>, kNumBands> gainSmoother2{};
    std::array<bool, kNumBands> lastEnabled{};

    MultibandEq eq;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetaPluginProcessor)
};
