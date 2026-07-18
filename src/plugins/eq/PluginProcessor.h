#pragma once

#include <array>
#include <atomic>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "dsp/MultibandEq.h"
#include "plugins/eq/SpectrumFifo.h"

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

    // Host sample rate, published by prepareToPlay for the editor's curve
    // view. The drawn curve must be evaluated at the ACTUAL fs — the bilinear
    // response near Nyquist depends on it (several dB of cramping for HF bands
    // at 44.1k). Atomic so the message-thread reader needs no lock against the
    // prepare/audio threads. 0.0 = not prepared yet (editor falls back to 48k).
    std::atomic<double> publishedSampleRate{0.0};

    static constexpr int kNumBands = MultibandEq::kNumBands;

    // Pre-EQ and post-EQ analyzer taps. Written on the audio thread in
    // processBlock (lock-free, drop-on-overflow), drained by the editor's
    // AnalyzerView on the message thread. Public so the editor can read them;
    // capacity is fixed in prepareToPlay (see kAnalyzerMaxFs) so the storage the
    // reader touches is never reallocated on a session sample-rate change.
    SpectrumFifo preFifo;
    SpectrumFifo postFifo;

    // Per-band dynamic gain OFFSET (signed dB), published from processBlock after
    // the EQ runs (plain float store, relaxed). The editor's curve view reads
    // these at paint rate to draw each dynamic band's live excursion. dsp/ stays
    // atomic-free — same publish pattern as the compressor's GR meter.
    std::array<std::atomic<float>, kNumBands> bandOffsetDb{};

private:
    static constexpr int kAnalyzerFftSize = 4096;
    static constexpr double kAnalyzerMaxFs = 192000.0;
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

    // --- Dynamic-EQ params (per band). Block-polled, NOT smoothed: they feed
    // the detector (threshold/ballistics) and the gain-computer, none of which is
    // the audio path directly — a per-block step in them is a slow control change
    // the detector's own ballistics already smooth, so there is no zipper risk. ---
    std::array<std::atomic<float>*, kNumBands> bandDynOn{};
    std::array<std::atomic<float>*, kNumBands> bandDynRange{};
    std::array<std::atomic<float>*, kNumBands> bandDynThresh{};
    std::array<std::atomic<float>*, kNumBands> bandDynAttack{};
    std::array<std::atomic<float>*, kNumBands> bandDynRelease{};

    std::array<juce::LinearSmoothedValue<double>, kNumBands> freqSmoother{};
    std::array<juce::LinearSmoothedValue<double>, kNumBands> qSmoother{};
    std::array<juce::LinearSmoothedValue<double>, kNumBands> gainSmoother2{};
    std::array<bool, kNumBands> lastEnabled{};

    MultibandEq eq;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetaPluginProcessor)
};
