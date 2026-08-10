#pragma once

#include <array>
#include <atomic>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "dsp/SpectralNode.h"

/**
    LpngSpectral — spectral dynamic-resonance suppressor (soothe-class) product.
    Thin plugin shell over the JUCE-free SpectralNode (src/dsp/): APVTS params
    -> once-per-block smoothed Params -> node. GenericAudioProcessorEditor for
    v1 (custom UI later).

    FIRST nonzero-latency product in the project: the node delays wet audio by
    exactly N=1024 samples (its STFT frame size), so the dry path is run
    through a matching N-sample ring-buffer delay before the dry/wet blend —
    see the ring buffer members below and processBlock()'s mix stage.
*/
class SpectralProcessor : public juce::AudioProcessor {
public:
    SpectralProcessor();

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
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

    // Sample rate mirrored from prepareToPlay for the UI thread (trace
    // displays later) — mirrors the Comp/EQ publishedSampleRate pattern.
    std::atomic<double> publishedSampleRate{0.0};

    double getPublishedSampleRate() const {
        return publishedSampleRate.load(std::memory_order_relaxed);
    }

private:
    std::atomic<float>* depthParam = nullptr;
    std::atomic<float>* detailParam = nullptr;
    std::atomic<float>* attackParam = nullptr;
    std::atomic<float>* releaseParam = nullptr;
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* outputParam = nullptr;

    juce::LinearSmoothedValue<double> depthSmoother;
    juce::LinearSmoothedValue<double> detailSmoother;
    juce::LinearSmoothedValue<double> attackSmoother;
    juce::LinearSmoothedValue<double> releaseSmoother;
    juce::LinearSmoothedValue<double> mixSmoother;
    juce::LinearSmoothedValue<double> outputSmoother;

    SpectralNode spectral;

    // Dry-delay ring: aligns the dry path to the node's fixed N=1024-sample
    // wet latency before the dry/wet blend. Allocated in prepareToPlay only;
    // read-then-write per sample in processBlock (no allocation there).
    std::array<std::vector<float>, 2> dryRing;
    int dryRingWritePos = 0;

    // Preallocated dry scratch, sized (numChannels, samplesPerBlock) in
    // prepareToPlay, filled per block in processBlock.
    juce::AudioBuffer<float> dryScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralProcessor)
};
