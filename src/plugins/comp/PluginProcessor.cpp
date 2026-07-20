#include "PluginProcessor.h"

#include <cmath>

#include "dsp/CompressorSchema.h"
#include "plugins/comp/ui/CompEditor.h"

#if JUCE_DEBUG
namespace {
// Runtime drift guard: walks the ACTUAL, live apvts and checks it against
// dsp/CompressorSchema.h's compressorSchema() table — id-for-id, catching
// any future edit to createParameterLayout() that isn't mirrored in the
// schema (or vice versa). Debug-only; this is a constructor-time (off audio
// thread) sanity check, not part of the shipped binary's hot path.
// param_schema_test.cpp is the complementary offline half of this guard.
void verifyCompressorSchemaAgainstApvts(juce::AudioProcessorValueTreeState& apvts) {
    using paramschema::ParamType;

    for (const auto& d : compressorschema::compressorSchema()) {
        auto* param = apvts.getParameter(d.id);
        jassert(param != nullptr);
        if (param == nullptr)
            continue;

        if (d.type == ParamType::Bool) {
            auto* b = dynamic_cast<juce::AudioParameterBool*>(param);
            jassert(b != nullptr);
            if (b != nullptr)
                jassert(b->get() == (d.defaultVal > 0.5));
        } else if (d.type == ParamType::Choice) {
            jassertfalse;  // compressor has no choice params
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

namespace {
// Block max |sample| across all channels, converted to dBFS. -100 dB is the
// published floor for a silent (all-zero) block, since 20*log10(0) is -inf
// and a meter has no use for an unbounded value. Bounded double pass over
// the buffer, no allocation — safe to call from processBlock.
float blockPeakDb(const juce::AudioBuffer<float>& buffer) {
    float peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        peak = std::max(peak, buffer.getMagnitude(ch, 0, buffer.getNumSamples()));
    return juce::Decibels::gainToDecibels(peak, -100.0f);
}
}  // namespace

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

#if JUCE_DEBUG
    verifyCompressorSchemaAgainstApvts(apvts);
#endif
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

    publishedSampleRate.store(sampleRate, std::memory_order_relaxed);

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

    // Input peak BEFORE the node touches the buffer.
    inputPeakDb.store(blockPeakDb(buffer), std::memory_order_relaxed);

    comp.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), numSamples);

    // Output peak AFTER the node processes.
    outputPeakDb.store(blockPeakDb(buffer), std::memory_order_relaxed);

    // Publish the block's worst GR + detector level for the editor (plain
    // double reads from the node, stored to atomics here — dsp/ stays
    // atomic-free). Two relaxed stores, no allocation/locks/IO.
    gainReductionDb.store(static_cast<float>(comp.getMaxGrDb()), std::memory_order_relaxed);
    scLevelDb.store(static_cast<float>(comp.getLastScDb()), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* CompProcessor::createEditor() {
    return new CompEditor(*this);
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
