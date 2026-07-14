#include "PluginProcessor.h"

MetaPluginProcessor::MetaPluginProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout()) {
    gainParamDb = apvts.getRawParameterValue("gain");
}

juce::AudioProcessorValueTreeState::ParameterLayout MetaPluginProcessor::createParameterLayout() {
    return {std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"gain", 1},
        "Gain",
        juce::NormalisableRange<float>(-24.0f, 12.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB"))};
}

void MetaPluginProcessor::prepareToPlay(double sampleRate, int) {
    gainSmoother.reset(sampleRate, 0.02);
    gainSmoother.setCurrentAndTargetValue(
        juce::Decibels::decibelsToGain(gainParamDb->load()));
}

void MetaPluginProcessor::releaseResources() {}

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

    gainSmoother.setTargetValue(juce::Decibels::decibelsToGain(gainParamDb->load()));
    gainSmoother.applyGain(buffer, buffer.getNumSamples());
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
