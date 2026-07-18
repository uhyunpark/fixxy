#include "PluginProcessor.h"

#include <cmath>

#include "dsp/EqSchema.h"
#include "plugins/eq/ui/EqEditor.h"

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

#if JUCE_DEBUG
// Runtime drift guard: walks the ACTUAL, live apvts and checks it against
// dsp/EqSchema.h's outputGainSchema()/bandSchema() tables — id-for-id (with
// the "<N>" template mechanically expanded per band), catching any future
// edit to createParameterLayout() that isn't mirrored in the schema (or vice
// versa). Debug-only; constructor-time (off audio thread) check, not part of
// the shipped binary's hot path. param_schema_test.cpp is the complementary
// offline half of this guard.
void verifyEqSchemaAgainstApvts(juce::AudioProcessorValueTreeState& apvts) {
    using paramschema::ParamDescriptor;
    using paramschema::ParamType;

    auto checkOne = [](juce::RangedAudioParameter* param, const ParamDescriptor& d,
                        double expectedDefault) {
        jassert(param != nullptr);
        if (param == nullptr)
            return;

        if (d.type == ParamType::Bool) {
            auto* b = dynamic_cast<juce::AudioParameterBool*>(param);
            jassert(b != nullptr);
            if (b != nullptr)
                jassert(b->get() == (expectedDefault > 0.5));
        } else if (d.type == ParamType::Choice) {
            auto* c = dynamic_cast<juce::AudioParameterChoice*>(param);
            jassert(c != nullptr);
            if (c != nullptr) {
                jassert(c->choices.size() == static_cast<int>(d.choices.size()));
                for (int i = 0; i < c->choices.size(); ++i)
                    jassert(c->choices[i] == juce::String(d.choices[static_cast<std::size_t>(i)]));
                jassert(c->getIndex() == static_cast<int>(std::lround(expectedDefault)));
            }
        } else {
            auto* f = dynamic_cast<juce::AudioParameterFloat*>(param);
            jassert(f != nullptr);
            if (f != nullptr) {
                const auto& r = f->getNormalisableRange();
                jassert(std::abs((double) r.start - d.min) < 1e-6);
                jassert(std::abs((double) r.end - d.max) < 1e-6);
                jassert(std::abs((double) f->get() - expectedDefault) < 1e-4);
            }
        }
    };

    checkOne(apvts.getParameter("gain"), eqschema::outputGainSchema(), 0.0);

    const auto band = eqschema::bandSchema();
    for (int i = 0; i < MultibandEq::kNumBands; ++i) {
        for (const auto& d : band) {
            // "band<N>Freq" -> the actual APVTS id via the SAME bandID() the
            // real layout was built with (not a re-derivation).
            const juce::String suffix = juce::String(d.id).fromLastOccurrenceOf("N>", false, false);
            auto* param = apvts.getParameter(bandID(i, suffix.toRawUTF8()));
            const double expectedDefault =
                (!d.defaultsByIndex.empty())
                    ? d.defaultsByIndex[static_cast<std::size_t>(i)]
                    : d.defaultVal;
            checkOne(param, d, expectedDefault);
        }
    }
}
#endif
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
        bandDynOn[static_cast<std::size_t>(i)] = apvts.getRawParameterValue(bandID(i, "DynOn"));
        bandDynRange[static_cast<std::size_t>(i)] = apvts.getRawParameterValue(bandID(i, "DynRange"));
        bandDynThresh[static_cast<std::size_t>(i)] = apvts.getRawParameterValue(bandID(i, "DynThresh"));
        bandDynAttack[static_cast<std::size_t>(i)] = apvts.getRawParameterValue(bandID(i, "DynAttack"));
        bandDynRelease[static_cast<std::size_t>(i)] = apvts.getRawParameterValue(bandID(i, "DynRelease"));
    }

#if JUCE_DEBUG
    verifyEqSchemaAgainstApvts(apvts);
#endif
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

        // Dynamic EQ (level-dependent band gain). Applies to the gain-bearing
        // matched types only (Bell/Low Shelf/High Shelf/Tilt); ignored otherwise.
        layout.add(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{bandID(i, "DynOn"), ver}, label + "Dyn", false));

        // Signed range: the SIGN sets direction (boost vs cut), the magnitude the
        // amount — one control folds both, so +/- are pure mirror images.
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{bandID(i, "DynRange"), ver}, label + "DynRange",
            juce::NormalisableRange<float>(-30.0f, 30.0f, 0.1f), 0.0f,
            juce::AudioParameterFloatAttributes().withLabel("dB")));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{bandID(i, "DynThresh"), ver}, label + "DynThresh",
            juce::NormalisableRange<float>(-60.0f, 0.0f, 0.1f), -24.0f,
            juce::AudioParameterFloatAttributes().withLabel("dB")));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{bandID(i, "DynAttack"), ver}, label + "DynAtt",
            juce::NormalisableRange<float>(0.05f, 100.0f, 0.01f, 0.4f), 2.0f,
            juce::AudioParameterFloatAttributes().withLabel("ms")));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{bandID(i, "DynRelease"), ver}, label + "DynRel",
            juce::NormalisableRange<float>(5.0f, 1000.0f, 0.1f, 0.4f), 80.0f,
            juce::AudioParameterFloatAttributes().withLabel("ms")));
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

    // Dynamic-EQ params: block-polled straight from the atomics (no smoother).
    // They drive the detector/gain-computer, not the audio path, so a per-block
    // step is harmless — the detector's ballistics already smooth the response.
    p.dynEnabled = bandDynOn[idx]->load() > 0.5f;
    p.dynRangeDb = bandDynRange[idx]->load();
    p.dynThresholdDb = bandDynThresh[idx]->load();
    p.dynAttackMs = bandDynAttack[idx]->load();
    p.dynReleaseMs = bandDynRelease[idx]->load();
    return p;
}

void MetaPluginProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    publishedSampleRate.store(sampleRate, std::memory_order_relaxed);

    gainSmoother.reset(sampleRate, 0.02);
    gainSmoother.setCurrentAndTargetValue(
        juce::Decibels::decibelsToGain(gainParamDb->load()));

    eq.prepare(sampleRate, samplesPerBlock, getTotalNumOutputChannels());

    // Analyzer taps. Capacity = max(2 * FFT, 0.1 s) at the HIGHEST supported
    // rate, so it satisfies the design's max(2*4096, ~0.1*fs) for the actual fs
    // and never reallocates on a later rate change while the editor reads it.
    const int fifoCap = juce::jmax(2 * kAnalyzerFftSize,
                                   static_cast<int>(std::ceil(0.1 * kAnalyzerMaxFs)));
    preFifo.prepare(fifoCap, getTotalNumOutputChannels());
    postFifo.prepare(fifoCap, getTotalNumOutputChannels());

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
        p.dynEnabled = bandDynOn[idx]->load() > 0.5f;
        p.dynRangeDb = bandDynRange[idx]->load();
        p.dynThresholdDb = bandDynThresh[idx]->load();
        p.dynAttackMs = bandDynAttack[idx]->load();
        p.dynReleaseMs = bandDynRelease[idx]->load();
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

    // 0) Pre-EQ tap. Capture the INPUT before eq.process() overwrites the buffer
    //    in place. Lock-free, bounded, drop-on-overflow (see SpectrumFifo::write).
    preFifo.write(buffer.getArrayOfReadPointers(), buffer.getNumChannels(), numSamples);

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

    // 3b) Publish each band's dynamic gain offset for the editor (plain float
    //     store, relaxed — dsp/ stays atomic-free; the node exposes a plain read).
    for (int i = 0; i < kNumBands; ++i)
        bandOffsetDb[static_cast<std::size_t>(i)].store(
            static_cast<float>(eq.getBandOffsetDb(i)), std::memory_order_relaxed);

    // 4) Output trim.
    gainSmoother.setTargetValue(juce::Decibels::decibelsToGain(gainParamDb->load()));
    gainSmoother.applyGain(buffer, numSamples);

    // 5) Post-EQ tap: the processed output after the trim.
    postFifo.write(buffer.getArrayOfReadPointers(), buffer.getNumChannels(), numSamples);
}

juce::AudioProcessorEditor* MetaPluginProcessor::createEditor() {
    return new EqEditor(*this);
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
