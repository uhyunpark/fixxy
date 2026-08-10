#include "PluginProcessor.h"

#include <cmath>

#include "dsp/SpectralSchema.h"

#if JUCE_DEBUG
namespace {
// Runtime drift guard: walks the ACTUAL, live apvts and checks it against
// dsp/SpectralSchema.h's spectralSchema() table — id-for-id, catching any
// future edit to createParameterLayout() that isn't mirrored in the schema
// (or vice versa). Debug-only; this is a constructor-time (off audio thread)
// sanity check, not part of the shipped binary's hot path.
// param_schema_test.cpp is the complementary offline half of this guard.
void verifySpectralSchemaAgainstApvts(juce::AudioProcessorValueTreeState& apvts) {
    using paramschema::ParamType;

    for (const auto& d : spectralschema::spectralSchema()) {
        auto* param = apvts.getParameter(d.id);
        jassert(param != nullptr);
        if (param == nullptr)
            continue;

        if (d.type == ParamType::Bool) {
            jassertfalse;  // spectral has no bool params
        } else if (d.type == ParamType::Choice) {
            jassertfalse;  // spectral has no choice params
        } else {
            auto* f = dynamic_cast<juce::AudioParameterFloat*>(param);
            jassert(f != nullptr);
            if (f != nullptr) {
                const auto& r = f->getNormalisableRange();
                jassert(std::abs((double) r.start - d.min) < 1e-6);
                jassert(std::abs((double) r.end - d.max) < 1e-6);
                jassert(std::abs((double) f->get() - d.defaultVal) < 1e-6);
            }
        }
    }
}
}  // namespace
#endif

SpectralProcessor::SpectralProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout()) {
    depthParam = apvts.getRawParameterValue("depth");
    detailParam = apvts.getRawParameterValue("detail");
    attackParam = apvts.getRawParameterValue("attack");
    releaseParam = apvts.getRawParameterValue("release");
    mixParam = apvts.getRawParameterValue("mix");
    outputParam = apvts.getRawParameterValue("output");

#if JUCE_DEBUG
    verifySpectralSchemaAgainstApvts(apvts);
#endif
}

juce::AudioProcessorValueTreeState::ParameterLayout SpectralProcessor::createParameterLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    using Float = juce::AudioParameterFloat;
    using Attr = juce::AudioParameterFloatAttributes;

    layout.add(std::make_unique<Float>(
        juce::ParameterID{"depth", 1}, "Depth",
        juce::NormalisableRange<float>(0.0f, 10.0f, 0.01f), 5.0f));

    layout.add(std::make_unique<Float>(
        juce::ParameterID{"detail", 1}, "Detail",
        juce::NormalisableRange<float>(0.0f, 10.0f, 0.01f), 5.0f));

    juce::NormalisableRange<float> attackRange(1.0f, 250.0f, 0.1f);
    attackRange.setSkewForCentre(27.0f);
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"attack", 1}, "Attack", attackRange, 27.0f, Attr().withLabel("ms")));

    juce::NormalisableRange<float> releaseRange(5.0f, 1000.0f, 0.1f);
    releaseRange.setSkewForCentre(100.0f);
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"release", 1}, "Release", releaseRange, 50.0f, Attr().withLabel("ms")));

    // MIX: 0..100% dry/wet, default 100 (fully wet). Dry path is delayed by
    // N=1024 samples to align with the node's fixed wet latency before this
    // blend (see processBlock()).
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"mix", 1}, "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f, Attr().withLabel("%")));

    layout.add(std::make_unique<Float>(
        juce::ParameterID{"output", 1}, "Output",
        juce::NormalisableRange<float>(-24.0f, 12.0f, 0.1f), 0.0f, Attr().withLabel("dB")));

    return layout;
}

void SpectralProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    // FIRST nonzero-latency product in the project. N=1024 (SpectralNode::
    // kFftSize) is fixed in v1, so latency is reported once here; if N ever
    // becomes a parameter, latency must be re-reported per prepareToPlay /
    // whenever N changes.
    setLatencySamples(SpectralNode::latencySamples());

    spectral.prepare(sampleRate, samplesPerBlock, getTotalNumOutputChannels());
    spectral.reset();

    publishedSampleRate.store(sampleRate, std::memory_order_relaxed);

    const double ramp = 0.02;  // 20 ms, mirrors the Comp/EQ boundary smoothing
    depthSmoother.reset(sampleRate, ramp);
    detailSmoother.reset(sampleRate, ramp);
    attackSmoother.reset(sampleRate, ramp);
    releaseSmoother.reset(sampleRate, ramp);
    mixSmoother.reset(sampleRate, ramp);
    outputSmoother.reset(sampleRate, ramp);

    depthSmoother.setCurrentAndTargetValue(depthParam->load());
    detailSmoother.setCurrentAndTargetValue(detailParam->load());
    attackSmoother.setCurrentAndTargetValue(attackParam->load());
    releaseSmoother.setCurrentAndTargetValue(releaseParam->load());
    mixSmoother.setCurrentAndTargetValue(mixParam->load());
    outputSmoother.setCurrentAndTargetValue(outputParam->load());

    const int numChannels = getTotalNumOutputChannels();
    for (int ch = 0; ch < 2; ++ch) {
        dryRing[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(SpectralNode::kFftSize), 0.0f);
    }
    dryRingWritePos = 0;

    dryScratch.setSize(numChannels, samplesPerBlock, false, false, true);
    dryScratch.clear();
}

void SpectralProcessor::releaseResources() {}

bool SpectralProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void SpectralProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // Advance the continuous smoothers by this block and resolve params ONCE
    // (setParams does the coefficient work). skip() returns the value after
    // advancing numSamples steps of the 20 ms ramp — no per-sample recompute.
    depthSmoother.setTargetValue(depthParam->load());
    detailSmoother.setTargetValue(detailParam->load());
    attackSmoother.setTargetValue(attackParam->load());
    releaseSmoother.setTargetValue(releaseParam->load());
    mixSmoother.setTargetValue(mixParam->load());
    outputSmoother.setTargetValue(outputParam->load());

    SpectralNode::Params p;
    p.depth = depthSmoother.skip(numSamples);
    p.detail = detailSmoother.skip(numSamples);
    p.attackMs = attackSmoother.skip(numSamples);
    p.releaseMs = releaseSmoother.skip(numSamples);
    spectral.setParams(p);

    const double mix = mixSmoother.skip(numSamples) / 100.0;      // percent -> [0..1]
    const double outputGainLinear = std::pow(10.0, outputSmoother.skip(numSamples) / 20.0);

    const int ringSize = SpectralNode::kFftSize;

    // Latency-aligned dry/wet: the node delays wet by exactly N=kFftSize
    // samples, so the dry path is run through a matching N-sample ring-buffer
    // delay (read-then-write single ring) before the blend.
    for (int ch = 0; ch < numChannels; ++ch) {
        auto& ring = dryRing[static_cast<std::size_t>(ch)];
        const float* in = buffer.getReadPointer(ch);
        float* dry = dryScratch.getWritePointer(ch);
        int pos = dryRingWritePos;
        for (int i = 0; i < numSamples; ++i) {
            dry[i] = ring[static_cast<std::size_t>(pos)];
            ring[static_cast<std::size_t>(pos)] = in[i];
            pos = (pos + 1) % ringSize;
        }
    }
    dryRingWritePos = (dryRingWritePos + numSamples) % ringSize;

    spectral.process(buffer.getArrayOfWritePointers(), numChannels, numSamples);

    for (int ch = 0; ch < numChannels; ++ch) {
        const float* dry = dryScratch.getReadPointer(ch);
        float* wet = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i) {
            const double out = ((1.0 - mix) * static_cast<double>(dry[i])
                                 + mix * static_cast<double>(wet[i])) * outputGainLinear;
            wet[i] = static_cast<float>(out);
        }
    }
}

juce::AudioProcessorEditor* SpectralProcessor::createEditor() {
    return new juce::GenericAudioProcessorEditor(*this);
}

void SpectralProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (auto state = apvts.copyState(); auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void SpectralProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(apvts.state.getType()))
        return;
    apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new SpectralProcessor();
}
