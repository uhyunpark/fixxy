#include "PluginProcessor.h"

namespace {
// Choice order MUST match SvfBandNode::Type.
const juce::StringArray kTypeChoices{
    "Bell", "Low Shelf", "High Shelf", "Tilt", "Notch", "Bandpass", "Lowpass", "Highpass"};

// Choice order MUST match kSlopeValues.
const juce::StringArray kSlopeChoices{
    "6", "12", "18", "24", "30", "36", "48", "72", "96"};
constexpr std::array<double, 9> kSlopeValues{6, 12, 18, 24, 30, 36, 48, 72, 96};
constexpr int kDefaultSlopeIndex = 1;  // 12 dB/oct

// Pro-Q-style log-spread default centre frequencies across the 16 bands.
constexpr std::array<float, 16> kDefaultFreqs{
    30.f, 50.f, 80.f, 120.f, 180.f, 260.f, 400.f, 600.f,
    900.f, 1400.f, 2000.f, 3000.f, 4500.f, 7000.f, 10000.f, 15000.f};

juce::String bandID(int i, const char* suffix) {
    return "band" + juce::String(i + 1) + suffix;
}
}  // namespace

MetaPluginProcessor::MetaPluginProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout()) {
    gainParamDb = apvts.getRawParameterValue("gain");
    for (int i = 0; i < kNumBands; ++i) {
        bandEnabled[static_cast<std::size_t>(i)] = apvts.getRawParameterValue(bandID(i, "Enabled"));
        bandType[static_cast<std::size_t>(i)] = apvts.getRawParameterValue(bandID(i, "Type"));
        bandFreq[static_cast<std::size_t>(i)] = apvts.getRawParameterValue(bandID(i, "Freq"));
        bandQ[static_cast<std::size_t>(i)] = apvts.getRawParameterValue(bandID(i, "Q"));
        bandGain[static_cast<std::size_t>(i)] = apvts.getRawParameterValue(bandID(i, "Gain"));
        bandSlope[static_cast<std::size_t>(i)] = apvts.getRawParameterValue(bandID(i, "Slope"));
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout MetaPluginProcessor::createParameterLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Output trim (was the Week-1 gain param).
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"gain", 1},
        "Output",
        juce::NormalisableRange<float>(-24.0f, 12.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    for (int i = 0; i < kNumBands; ++i) {
        const int ver = 1;
        const juce::String label = "B" + juce::String(i + 1) + " ";

        layout.add(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{bandID(i, "Enabled"), ver}, label + "On", false));

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{bandID(i, "Type"), ver}, label + "Type", kTypeChoices, 0));

        // 20 Hz–20 kHz, log skew so an octave is a constant knob distance.
        juce::NormalisableRange<float> freqRange(20.0f, 20000.0f, 1.0f);
        freqRange.setSkewForCentre(1000.0f);
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{bandID(i, "Freq"), ver}, label + "Freq",
            freqRange, kDefaultFreqs[static_cast<std::size_t>(i)],
            juce::AudioParameterFloatAttributes().withLabel("Hz")));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{bandID(i, "Q"), ver}, label + "Q",
            juce::NormalisableRange<float>(0.1f, 18.0f, 0.01f, 0.5f), 0.7071f));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{bandID(i, "Gain"), ver}, label + "Gain",
            juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f,
            juce::AudioParameterFloatAttributes().withLabel("dB")));

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{bandID(i, "Slope"), ver}, label + "Slope",
            kSlopeChoices, kDefaultSlopeIndex));
    }

    return layout;
}

SvfBandNode::BandParams MetaPluginProcessor::readBandParams(int i, int numSamples) {
    const auto idx = static_cast<std::size_t>(i);
    freqSmoother[idx].setTargetValue(bandFreq[idx]->load());
    qSmoother[idx].setTargetValue(bandQ[idx]->load());
    gainSmoother2[idx].setTargetValue(bandGain[idx]->load());

    SvfBandNode::BandParams p;
    const int typeIdx = static_cast<int>(bandType[idx]->load());
    p.type = static_cast<SvfBandNode::Type>(typeIdx);
    p.f0 = freqSmoother[idx].skip(numSamples);
    p.q = qSmoother[idx].skip(numSamples);
    p.gainDb = gainSmoother2[idx].skip(numSamples);
    const int slopeIdx = juce::jlimit(0, static_cast<int>(kSlopeValues.size()) - 1,
                                      static_cast<int>(bandSlope[idx]->load()));
    p.slope = kSlopeValues[static_cast<std::size_t>(slopeIdx)];
    return p;
}

void MetaPluginProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    gainSmoother.reset(sampleRate, 0.02);
    gainSmoother.setCurrentAndTargetValue(
        juce::Decibels::decibelsToGain(gainParamDb->load()));

    eq.prepare(sampleRate, samplesPerBlock, getTotalNumOutputChannels());

    for (int i = 0; i < kNumBands; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        // 20 ms ramps; with ~2.9 ms blocks the per-block coeff step is small and
        // the SVF absorbs it without zipper.
        freqSmoother[idx].reset(sampleRate, 0.02);
        qSmoother[idx].reset(sampleRate, 0.02);
        gainSmoother2[idx].reset(sampleRate, 0.02);
        freqSmoother[idx].setCurrentAndTargetValue(bandFreq[idx]->load());
        qSmoother[idx].setCurrentAndTargetValue(bandQ[idx]->load());
        gainSmoother2[idx].setCurrentAndTargetValue(bandGain[idx]->load());

        SvfBandNode::BandParams p;
        p.type = static_cast<SvfBandNode::Type>(static_cast<int>(bandType[idx]->load()));
        p.f0 = bandFreq[idx]->load();
        p.q = bandQ[idx]->load();
        p.gainDb = bandGain[idx]->load();
        const int slopeIdx = juce::jlimit(0, static_cast<int>(kSlopeValues.size()) - 1,
                                          static_cast<int>(bandSlope[idx]->load()));
        p.slope = kSlopeValues[static_cast<std::size_t>(slopeIdx)];
        eq.setBandParams(i, p);

        const bool en = bandEnabled[idx]->load() > 0.5f;
        eq.setBandEnabled(i, en, /*immediate=*/true);
        lastEnabled[idx] = en;
    }
    eq.reset();
}

void MetaPluginProcessor::releaseResources() {}

void MetaPluginProcessor::reset() {
    eq.reset();
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

    // 1) Detect enable/disable edges -> trigger the 10 ms crossfade. Cheap:
    //    an atomic load + bool compare per band, no coeff work here.
    for (int i = 0; i < kNumBands; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        const bool en = bandEnabled[idx]->load() > 0.5f;
        if (en != lastEnabled[idx]) {
            eq.setBandEnabled(i, en);
            lastEnabled[idx] = en;
        }
    }

    // 2) Advance smoothers and recompute coefficients ONCE per block, only for
    //    active (enabled or still fading) bands. Inactive bands snap their
    //    smoothers to the current target (no tan(), no section loop).
    for (int i = 0; i < kNumBands; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        if (eq.isBandActive(i)) {
            eq.setBandParams(i, readBandParams(i, numSamples));
        } else {
            freqSmoother[idx].setCurrentAndTargetValue(bandFreq[idx]->load());
            qSmoother[idx].setCurrentAndTargetValue(bandQ[idx]->load());
            gainSmoother2[idx].setCurrentAndTargetValue(bandGain[idx]->load());
        }
    }

    // 3) Run the 16-band cascade (disabled bands skipped inside).
    eq.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), numSamples);

    // 4) Output trim.
    gainSmoother.setTargetValue(juce::Decibels::decibelsToGain(gainParamDb->load()));
    gainSmoother.applyGain(buffer, numSamples);
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
