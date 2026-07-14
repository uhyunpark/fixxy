#include "PluginProcessor.h"

MetaPluginProcessor::MetaPluginProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout()) {
    gainParamDb = apvts.getRawParameterValue("gain");
    eqFreqParam = apvts.getRawParameterValue("eqFreq");
    eqQParam = apvts.getRawParameterValue("eqQ");
    eqGainParam = apvts.getRawParameterValue("eqGain");
}

juce::AudioProcessorValueTreeState::ParameterLayout MetaPluginProcessor::createParameterLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"gain", 1},
        "Gain",
        juce::NormalisableRange<float>(-24.0f, 12.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    // EQ frequency: 20 Hz–20 kHz on a log skew so the control is perceptually
    // even (an octave is the same knob distance everywhere).
    juce::NormalisableRange<float> freqRange(20.0f, 20000.0f, 1.0f);
    freqRange.setSkewForCentre(1000.0f);
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"eqFreq", 1},
        "EQ Freq",
        freqRange,
        1000.0f,
        juce::AudioParameterFloatAttributes().withLabel("Hz")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"eqQ", 1},
        "EQ Q",
        juce::NormalisableRange<float>(0.1f, 18.0f, 0.01f, 0.5f),
        1.0f));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"eqGain", 1},
        "EQ Gain",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    return layout;
}

void MetaPluginProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    gainSmoother.reset(sampleRate, 0.02);
    gainSmoother.setCurrentAndTargetValue(
        juce::Decibels::decibelsToGain(gainParamDb->load()));

    // Smoothers ramp over 20 ms; with 128-sample blocks (~2.9 ms) the per-block
    // coefficient steps are small and the SVF absorbs them without zipper.
    eqFreqSmoother.reset(sampleRate, 0.02);
    eqQSmoother.reset(sampleRate, 0.02);
    eqGainSmoother.reset(sampleRate, 0.02);
    eqFreqSmoother.setCurrentAndTargetValue(eqFreqParam->load());
    eqQSmoother.setCurrentAndTargetValue(eqQParam->load());
    eqGainSmoother.setCurrentAndTargetValue(eqGainParam->load());

    eqBand.prepare(sampleRate, samplesPerBlock);
    eqBand.setParams({eqFreqParam->load(), eqQParam->load(), eqGainParam->load()});
    eqBand.reset();
}

void MetaPluginProcessor::releaseResources() {}

void MetaPluginProcessor::reset() {
    // Host-invoked reset path: clear filter memory so a re-start doesn't ring
    // out stale state.
    eqBand.reset();
}

bool MetaPluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void MetaPluginProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();

    gainSmoother.setTargetValue(juce::Decibels::decibelsToGain(gainParamDb->load()));
    gainSmoother.applyGain(buffer, numSamples);

    // Advance the param smoothers by one block and resolve EQ coefficients ONCE
    // per block from the smoothed values. skip() advances the ramp by numSamples
    // and returns the end-of-block value — no per-sample tan()/division.
    eqFreqSmoother.setTargetValue(eqFreqParam->load());
    eqQSmoother.setTargetValue(eqQParam->load());
    eqGainSmoother.setTargetValue(eqGainParam->load());
    eqBand.setParams({eqFreqSmoother.skip(numSamples),
                      eqQSmoother.skip(numSamples),
                      eqGainSmoother.skip(numSamples)});
    eqBand.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), numSamples);
}

juce::AudioProcessorEditor* MetaPluginProcessor::createEditor() {
    return new juce::GenericAudioProcessorEditor(*this);
}

void MetaPluginProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (auto state = apvts.copyState(); auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void MetaPluginProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(apvts.state.getType()))
        return;
    apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new MetaPluginProcessor();
}
