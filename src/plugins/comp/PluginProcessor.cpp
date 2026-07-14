#include "PluginProcessor.h"

CompProcessor::CompProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout()) {
    thresholdParam = apvts.getRawParameterValue("threshold");
    ratioParam = apvts.getRawParameterValue("ratio");
    kneeParam = apvts.getRawParameterValue("knee");
    attackParam = apvts.getRawParameterValue("attack");
    releaseParam = apvts.getRawParameterValue("release");
    makeupParam = apvts.getRawParameterValue("makeup");
    mixParam = apvts.getRawParameterValue("mix");
    rangeParam = apvts.getRawParameterValue("range");
    holdParam = apvts.getRawParameterValue("hold");
    autoMakeupParam = apvts.getRawParameterValue("autoMakeup");
    progRelParam = apvts.getRawParameterValue("progRelease");
    scHpfParam = apvts.getRawParameterValue("scHpf");
    rmsParam = apvts.getRawParameterValue("rmsMode");
}

juce::AudioProcessorValueTreeState::ParameterLayout CompProcessor::createParameterLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    using Float = juce::AudioParameterFloat;
    using Bool = juce::AudioParameterBool;
    using Attr = juce::AudioParameterFloatAttributes;

    layout.add(std::make_unique<Float>(
        juce::ParameterID{"threshold", 1}, "Threshold",
        juce::NormalisableRange<float>(-60.0f, 0.0f, 0.1f), -20.0f, Attr().withLabel("dB")));

    // Ratio 1..20, skewed so the useful low ratios get more knob travel.
    juce::NormalisableRange<float> ratioRange(1.0f, 20.0f, 0.01f);
    ratioRange.setSkewForCentre(4.0f);
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"ratio", 1}, "Ratio", ratioRange, 4.0f, Attr().withLabel(":1")));

    layout.add(std::make_unique<Float>(
        juce::ParameterID{"knee", 1}, "Knee",
        juce::NormalisableRange<float>(0.0f, 24.0f, 0.1f), 6.0f, Attr().withLabel("dB")));

    juce::NormalisableRange<float> attackRange(0.0f, 200.0f, 0.001f);
    attackRange.setSkewForCentre(10.0f);
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"attack", 1}, "Attack", attackRange, 10.0f, Attr().withLabel("ms")));

    juce::NormalisableRange<float> releaseRange(1.0f, 2000.0f, 0.1f);
    releaseRange.setSkewForCentre(100.0f);
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"release", 1}, "Release", releaseRange, 100.0f, Attr().withLabel("ms")));

    layout.add(std::make_unique<Float>(
        juce::ParameterID{"makeup", 1}, "Makeup",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f, Attr().withLabel("dB")));

    // MIX: 0..100% dry/wet, default 100 (fully wet = classic serial comp). The
    // node wants [0..1], so processBlock divides by 100 after smoothing.
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"mix", 1}, "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f, Attr().withLabel("%")));

    // RANGE: 0..96 dB ceiling on applied gain reduction, default 96 = "Off". The
    // top of the range is the node's kRangeOffDb sentinel (unlimited); shown as
    // "Off" there, "N.N dB" elsewhere.
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"range", 1}, "Range",
        juce::NormalisableRange<float>(0.0f, 96.0f, 0.1f), 96.0f,
        Attr().withStringFromValueFunction([](float v, int) {
            return v >= 96.0f ? juce::String("Off") : juce::String(v, 1) + " dB";
        })));

    // HOLD: 0..500 ms, skewed toward the short end where it matters, default 0
    // (off). Discrete per block (see header).
    juce::NormalisableRange<float> holdRange(0.0f, 500.0f, 0.1f);
    holdRange.setSkewForCentre(50.0f);
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"hold", 1}, "Hold", holdRange, 0.0f, Attr().withLabel("ms")));

    layout.add(std::make_unique<Bool>(juce::ParameterID{"autoMakeup", 1}, "Auto Makeup", false));
    layout.add(std::make_unique<Bool>(juce::ParameterID{"progRelease", 1}, "Program Release", false));
    layout.add(std::make_unique<Bool>(juce::ParameterID{"scHpf", 1}, "SC HPF 100Hz", false));
    layout.add(std::make_unique<Bool>(juce::ParameterID{"rmsMode", 1}, "RMS Detect", false));

    return layout;
}

void CompProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    // NO lookahead in v1 -> zero latency, nothing to report.
    setLatencySamples(0);

    comp.prepare(sampleRate, samplesPerBlock, getTotalNumOutputChannels());

    const double ramp = 0.02;  // 20 ms, mirrors the EQ boundary smoothing
    thresholdSmoother.reset(sampleRate, ramp);
    ratioSmoother.reset(sampleRate, ramp);
    kneeSmoother.reset(sampleRate, ramp);
    attackSmoother.reset(sampleRate, ramp);
    releaseSmoother.reset(sampleRate, ramp);
    makeupSmoother.reset(sampleRate, ramp);
    mixSmoother.reset(sampleRate, ramp);
    rangeSmoother.reset(sampleRate, ramp);

    thresholdSmoother.setCurrentAndTargetValue(thresholdParam->load());
    ratioSmoother.setCurrentAndTargetValue(ratioParam->load());
    kneeSmoother.setCurrentAndTargetValue(kneeParam->load());
    attackSmoother.setCurrentAndTargetValue(attackParam->load());
    releaseSmoother.setCurrentAndTargetValue(releaseParam->load());
    makeupSmoother.setCurrentAndTargetValue(makeupParam->load());
    mixSmoother.setCurrentAndTargetValue(mixParam->load());
    rangeSmoother.setCurrentAndTargetValue(rangeParam->load());

    comp.reset();
}

void CompProcessor::releaseResources() {}

void CompProcessor::reset() {
    comp.reset();
}

bool CompProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void CompProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();

    // Advance the continuous smoothers by this block and resolve coefficients
    // ONCE (setParams does the exp()/pow() work). skip() returns the value after
    // advancing numSamples steps of the 20 ms ramp — no per-sample recompute.
    thresholdSmoother.setTargetValue(thresholdParam->load());
    ratioSmoother.setTargetValue(ratioParam->load());
    kneeSmoother.setTargetValue(kneeParam->load());
    attackSmoother.setTargetValue(attackParam->load());
    releaseSmoother.setTargetValue(releaseParam->load());
    makeupSmoother.setTargetValue(makeupParam->load());
    mixSmoother.setTargetValue(mixParam->load());
    rangeSmoother.setTargetValue(rangeParam->load());

    CompressorNode::Params p;
    p.thresholdDb = thresholdSmoother.skip(numSamples);
    p.ratio = ratioSmoother.skip(numSamples);
    p.kneeDb = kneeSmoother.skip(numSamples);
    p.attackMs = attackSmoother.skip(numSamples);
    p.releaseMs = releaseSmoother.skip(numSamples);
    p.makeupDb = makeupSmoother.skip(numSamples);
    p.mix = mixSmoother.skip(numSamples) / 100.0;  // percent -> [0..1]
    p.rangeDb = rangeSmoother.skip(numSamples);
    p.holdMs = holdParam->load();  // discrete, no smoothing
    p.autoMakeup = autoMakeupParam->load() > 0.5f;
    p.programDependentRelease = progRelParam->load() > 0.5f;
    p.scHpfOn = scHpfParam->load() > 0.5f;
    p.rmsMode = rmsParam->load() > 0.5f;
    comp.setParams(p);

    comp.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), numSamples);

    // Publish the block's worst GR for the meter (plain double read from the
    // node, stored to the atomic here — dsp/ stays atomic-free).
    gainReductionDb.store(static_cast<float>(comp.getMaxGrDb()), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* CompProcessor::createEditor() {
    return new juce::GenericAudioProcessorEditor(*this);
}

void CompProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (auto state = apvts.copyState(); auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void CompProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(apvts.state.getType()))
        return;
    apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new CompProcessor();
}
