#include "PluginProcessor.h"

#include <cmath>
#include <cstdint>

#include <mach/mach_time.h>  // FIELD DIAGNOSTIC (temporary): callback-load timing

#include "dsp/SpectralSchema.h"
#include "plugins/spectral/SpectralMix.h"
#include "plugins/spectral/ui/SpectralEditor.h"

namespace {

// Weight-curve band ids are MECHANICAL: "wband<i><Suffix>", i = 0..7
// (0-based, unlike the EQ's 1-based bandID) — the single place that string
// is built, so the layout and the schema/runtime-guard reads of it can't
// drift apart. Mirrors bandID() in plugins/eq/PluginProcessor.cpp.
juce::String wbandID(int i, const char* suffix) {
    return "wband" + juce::String(i) + suffix;
}

// Choice order MUST match svfcoeffs::Type (Bell=0, LowShelf, HighShelf,
// Tilt, Notch, Bandpass, Lowpass, Highpass) — see dsp/SpectralWeight.h.
const juce::StringArray kWeightTypeChoices{
    "bell", "low shelf", "high shelf", "tilt", "notch", "bandpass", "lowpass", "highpass"};

const juce::StringArray kDetectionModeChoices{"soft", "hard"};

constexpr std::array<float, spectralweight::kMaxWeightBands> kWbandDefaultFreqs{
    120.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 12000.f};

#if JUCE_DEBUG
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
            auto* b = dynamic_cast<juce::AudioParameterBool*>(param);
            jassert(b != nullptr);
            if (b != nullptr)
                jassert(std::abs((b->get() ? 1.0 : 0.0) - d.defaultVal) < 1e-6);
        } else if (d.type == ParamType::Choice) {
            auto* c = dynamic_cast<juce::AudioParameterChoice*>(param);
            jassert(c != nullptr);
            if (c != nullptr) {
                jassert(c->choices.size() == static_cast<int>(d.choices.size()));
                for (int i = 0; i < c->choices.size(); ++i)
                    jassert(c->choices[i] == juce::String(d.choices[static_cast<std::size_t>(i)]));
                jassert(c->getIndex() == static_cast<int>(std::lround(d.defaultVal)));
            }
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
#endif
}  // namespace

SpectralProcessor::SpectralProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", juce::AudioChannelSet::stereo(), false)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, &undoManager, "PARAMETERS", createParameterLayout()),
      workflow(apvts, undoManager) {
    depthParam = apvts.getRawParameterValue("depth");
    detailParam = apvts.getRawParameterValue("detail");
    attackParam = apvts.getRawParameterValue("attack");
    releaseParam = apvts.getRawParameterValue("release");
    mixParam = apvts.getRawParameterValue("mix");
    outputParam = apvts.getRawParameterValue("output");
    bypassParam = apvts.getRawParameterValue("bypass");
    deltaParam = apvts.getRawParameterValue("delta");

    maxCutParam = apvts.getRawParameterValue("maxCut");
    wetTrimParam = apvts.getRawParameterValue("wetTrim");
    msModeParam = apvts.getRawParameterValue("msMode");
    linkPctParam = apvts.getRawParameterValue("linkPct");
    focusParam = apvts.getRawParameterValue("focus");
    tiltDetailParam = apvts.getRawParameterValue("tiltDetail");
    tiltAttackParam = apvts.getRawParameterValue("tiltAttack");
    tiltReleaseParam = apvts.getRawParameterValue("tiltRelease");

    for (int i = 0; i < kNumWeightBands; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        wbandOn[idx] = apvts.getRawParameterValue(wbandID(i, "On"));
        wbandType[idx] = apvts.getRawParameterValue(wbandID(i, "Type"));
        wbandFreq[idx] = apvts.getRawParameterValue(wbandID(i, "Freq"));
        wbandGain[idx] = apvts.getRawParameterValue(wbandID(i, "Gain"));
        wbandQ[idx] = apvts.getRawParameterValue(wbandID(i, "Q"));
    }
    rangeLoParam = apvts.getRawParameterValue("rangeLo");
    rangeHiParam = apvts.getRawParameterValue("rangeHi");
    detectionModeParam = apvts.getRawParameterValue("detectionMode");
    externalSidechainParam = apvts.getRawParameterValue("externalSidechain");
    sidechainListenParam = apvts.getRawParameterValue("sidechainListen");

    // FIELD DIAGNOSTIC (temporary): cache the mach counter->ns ratio once, off
    // the audio thread. REMOVE with the callback-load readout.
    {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        machTimebaseNsPerTick = static_cast<double>(tb.numer) / static_cast<double>(tb.denom);
    }

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

    // BYPASS: transparent, latency-preserving passthrough (output = dry, aligned
    // to the N-sample delay). The node keeps running to preserve its history.
    // DELIBERATELY a plain parameter, NOT the host-bypass parameter (we do not
    // override getBypassParameter()): our pill only re-selects the output while
    // processBlock keeps taping specFifo + running the node, so the analyzer
    // stays live under our bypass. processBlockBypassed uses the same aligned
    // dry path when a host calls it; a host that stops callbacks entirely leaves
    // the editor to decay its display toward the floor.
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"bypass", 1}, "Bypass", false));

    // DELTA: solo the removed component (wet - dry). soothe's signature listening
    // tool AND our dev probe — ignores mix, applies output trim. depth=0 => silence.
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"delta", 1}, "Delta", false));

    // --- SIDEBAR globals (proto/spectral_sidebar.py). Version 1. Every default is
    // an exact no-op; the node collapses to the pre-sidebar shared-gain path and
    // wetTrim to unity there. ---

    // MAX CUT: ceiling on per-bin reduction, 1..24 dB; 24 = OFF (no clamp),
    // displayed "off" at the top of the range.
    juce::NormalisableRange<float> maxCutRange(1.0f, 24.0f, 0.1f);
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"maxCut", 1}, "Max Cut", maxCutRange, 24.0f,
        Attr().withLabel("dB").withStringFromValueFunction(
            [](float v, int) -> juce::String {
                return v >= 24.0f ? juce::String("off")
                                  : juce::String(v, 1) + " dB";
            })));

    // WET TRIM: flat gain on the WET component only, applied in the output blend
    // (SpectralMix.h), NOT in the node — keeps the node's identity/latency clean.
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"wetTrim", 1}, "Wet Trim",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f, Attr().withLabel("dB")));

    // M/S: process M/S instead of L/R (energy-preserving); mono => no-op.
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"msMode", 1}, "M/S Mode", false));

    // LINK: detection blend, 100 = fully linked common gain map (single envelope,
    // the pre-sidebar behaviour), 0 = per-channel independent.
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"linkPct", 1}, "Link",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f, Attr().withLabel("%")));

    // FOCUS: per-channel depth bias toward L/M (+) or R/S (-).
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"focus", 1}, "Focus",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));

    // TILT DETAIL / ATTACK / RELEASE: frequency tilt (around 1 kHz) of the per-bin
    // depth multiplier / attack time / release time.
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"tiltDetail", 1}, "Tilt Detail",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"tiltAttack", 1}, "Tilt Attack",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"tiltRelease", 1}, "Tilt Release",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));

    // WEIGHT CURVE: up to kMaxWeightBands user bands that bias the node's
    // per-bin detection, plus a [rangeLo, rangeHi] window that gates a band's
    // influence toward zero outside it (see dsp/SpectralWeight.h; pins at the
    // 20/20000 extremes = that edge fully off).
    for (int i = 0; i < spectralweight::kMaxWeightBands; ++i) {
        const int ver = 1;
        const juce::String label = "W" + juce::String(i + 1) + " ";

        layout.add(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{wbandID(i, "On"), ver}, label + "On", false));

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{wbandID(i, "Type"), ver}, label + "Type", kWeightTypeChoices, 0));

        // 20 Hz-20 kHz, log skew (mirrors the EQ band-freq convention).
        juce::NormalisableRange<float> freqRange(20.0f, 20000.0f, 1.0f);
        freqRange.setSkewForCentre(1000.0f);
        layout.add(std::make_unique<Float>(
            juce::ParameterID{wbandID(i, "Freq"), ver}, label + "Freq",
            freqRange, kWbandDefaultFreqs[static_cast<std::size_t>(i)], Attr().withLabel("Hz")));

        layout.add(std::make_unique<Float>(
            juce::ParameterID{wbandID(i, "Gain"), ver}, label + "Gain",
            juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f, Attr().withLabel("dB")));

        layout.add(std::make_unique<Float>(
            juce::ParameterID{wbandID(i, "Q"), ver}, label + "Q",
            juce::NormalisableRange<float>(0.3f, 12.0f, 0.01f, 0.5f), 0.7f));
    }

    juce::NormalisableRange<float> rangeLoRange(20.0f, 20000.0f, 1.0f);
    rangeLoRange.setSkewForCentre(1000.0f);
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"rangeLo", 1}, "Range Lo", rangeLoRange, 20.0f, Attr().withLabel("Hz")));

    juce::NormalisableRange<float> rangeHiRange(20.0f, 20000.0f, 1.0f);
    rangeHiRange.setSkewForCentre(1000.0f);
    layout.add(std::make_unique<Float>(
        juce::ParameterID{"rangeHi", 1}, "Range Hi", rangeHiRange, 20000.0f, Attr().withLabel("Hz")));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"detectionMode", 1}, "Detection Mode", kDetectionModeChoices, 0));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"externalSidechain", 1}, "External Sidechain", false));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sidechainListen", 1}, "Sidechain Listen", false));

    return layout;
}

void SpectralProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    // FIRST nonzero-latency product in the project. N=1024 (SpectralNode::
    // kFftSize) is fixed in v1, so latency is reported once here; if N ever
    // becomes a parameter, latency must be re-reported per prepareToPlay /
    // whenever N changes.
    setLatencySamples(SpectralNode::latencySamples());

    spectral.prepare(sampleRate, samplesPerBlock, getMainBusNumOutputChannels());
    spectral.reset();

    publishedSampleRate.store(sampleRate, std::memory_order_relaxed);

    const double ramp = 0.02;  // 20 ms, mirrors the Comp/EQ boundary smoothing
    depthSmoother.reset(sampleRate, ramp);
    detailSmoother.reset(sampleRate, ramp);
    attackSmoother.reset(sampleRate, ramp);
    releaseSmoother.reset(sampleRate, ramp);
    mixSmoother.reset(sampleRate, ramp);
    outputSmoother.reset(sampleRate, ramp);
    wetTrimSmoother.reset(sampleRate, ramp);

    depthSmoother.setCurrentAndTargetValue(depthParam->load());
    detailSmoother.setCurrentAndTargetValue(detailParam->load());
    attackSmoother.setCurrentAndTargetValue(attackParam->load());
    releaseSmoother.setCurrentAndTargetValue(releaseParam->load());
    mixSmoother.setCurrentAndTargetValue(mixParam->load());
    outputSmoother.setCurrentAndTargetValue(outputParam->load());
    wetTrimSmoother.setCurrentAndTargetValue(wetTrimParam->load());

    const int numChannels = getMainBusNumOutputChannels();
    for (int ch = 0; ch < 2; ++ch) {
        dryRing[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(SpectralNode::kFftSize), 0.0f);
        keyDelay[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(SpectralNode::kFftSize), 0.0f);
    }
    dryRingWritePos = 0;

    dryScratch.setSize(numChannels, juce::jmax(1, samplesPerBlock), false, false, true);
    dryScratch.clear();
    keyScratch.setSize(2, juce::jmax(1, samplesPerBlock), false, false, true);
    listenScratch.setSize(numChannels, juce::jmax(1, samplesPerBlock), false, false, true);
    keyScratch.clear();
    listenScratch.clear();
    sidechainLevelDb.store(-100.0f, std::memory_order_relaxed);
    using spectralmix::Mode;
    monitorRoute.prepare(sampleRate, bypassParam->load() > .5f ? Mode::Bypass
        : sidechainListenParam->load() > .5f ? Mode::ScListen
        : deltaParam->load() > .5f ? Mode::Delta : Mode::Normal);

    // Input spectrum tap. Capacity = max(2 * FFT, 0.1 s) at the highest
    // supported rate (mirrors the EQ analyzer sizing) so it never reallocates
    // under the editor's reader on a later sample-rate change.
    const int fifoCap = juce::jmax(2 * kAnalyzerFftSize,
                                   static_cast<int>(std::ceil(0.1 * kAnalyzerMaxFs)));
    specFifo.prepare(fifoCap, numChannels);
}

void SpectralProcessor::releaseResources() {}

bool SpectralProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    if (layouts.inputBuses.size() != 2 || layouts.outputBuses.size() != 1) return false;
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    const auto& key = layouts.getChannelSet(true, 1);
    return out == layouts.getMainInputChannelSet()
        && (key.isDisabled() || key == juce::AudioChannelSet::mono()
                            || key == juce::AudioChannelSet::stereo());
}

void SpectralProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    process(buffer, false);
}

void SpectralProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    process(buffer, true);
}

void SpectralProcessor::process(juce::AudioBuffer<float>& buffer, bool hostBypassed) {
    // FIELD DIAGNOSTIC (temporary): timestamp the callback entry. mach_absolute_time
    // is a userspace counter read (no syscall, bounded, audio-thread safe); the
    // matching read + relaxed atomic stores are at the end of this function. REMOVE
    // this and the block at the bottom once the crackle is diagnosed.
    const std::uint64_t diagT0 = mach_absolute_time();

    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    auto mainInput = getBusBuffer(buffer, true, 0);
    auto mainOutput = getBusBuffer(buffer, false, 0);
    auto keyInput = getBusBuffer(buffer, true, 1);
    const int numChannels = mainOutput.getNumChannels();
    const int keyChannels = keyInput.getNumChannels();

    // Bypass wins over delta (see spectralmix::Mode / SpectralMix.h): when both
    // pills are on, delta is suppressed for BOTH the mix mode and the analyzer
    // tap below, so the display and audio can never disagree within a block.
    const bool bypassActive = hostBypassed || bypassParam->load() > 0.5f;
    const bool listenActive = !bypassActive && sidechainListenParam->load() > .5f;
    const bool deltaActive = !bypassActive && !listenActive && (deltaParam->load() > 0.5f);

    // Input spectrum tap (pre-node): the editor's analyzer normally draws the
    // signal ENTERING the processor. Lock-free, bounded, drop-on-overflow
    // (SpectrumFifo::write) — the one added audio-thread publish besides the
    // reduction snapshot below. In DELTA mode the editor instead shows the
    // delta signal (wet - dry) — see the post-blend tap at the end of this
    // function, once `buffer` actually holds that signal.
    if (!deltaActive && !listenActive)
        specFifo.write(mainInput.getArrayOfReadPointers(), numChannels, numSamples);

    // Resolve the detector controls once per block. Output-blend controls below
    // advance once per sample so their 20 ms ramps do not become block steps.
    depthSmoother.setTargetValue(depthParam->load());
    detailSmoother.setTargetValue(detailParam->load());
    attackSmoother.setTargetValue(attackParam->load());
    releaseSmoother.setTargetValue(releaseParam->load());
    mixSmoother.setTargetValue(mixParam->load());
    outputSmoother.setTargetValue(outputParam->load());
    wetTrimSmoother.setTargetValue(wetTrimParam->load());

    SpectralNode::Params p;
    p.depth = depthSmoother.skip(numSamples);
    p.detail = detailSmoother.skip(numSamples);
    p.attackMs = attackSmoother.skip(numSamples);
    p.releaseMs = releaseSmoother.skip(numSamples);

    // Sidebar globals: block-polled straight from the atomics, NOT smoothed
    // (except wetTrim, resolved below in the output blend). Same rationale as the
    // weight bands — msMode/link/focus/tilt are discrete-ish routing/shape choices
    // and the node's own per-bin ballistics smooth any audible change; a boundary
    // smoother would just delay them. wetTrim IS smoothed because it is a
    // continuous output-level gesture, applied per sample in the blend.
    p.maxCutDb = static_cast<double>(maxCutParam->load());
    // wetTrimDb stays 0 in Params: the NODE never applies it — SpectralMix does.
    p.msMode = msModeParam->load() > 0.5f;
    p.linkPct = static_cast<double>(linkPctParam->load());
    p.focus = static_cast<double>(focusParam->load());
    p.tiltDetail = static_cast<double>(tiltDetailParam->load());
    p.tiltAttack = static_cast<double>(tiltAttackParam->load());
    p.tiltRelease = static_cast<double>(tiltReleaseParam->load());

    // Weight-curve bands and the range window feed the node's per-bin detection
    // weights; its attack/release ballistics smooth the resulting attenuation.
    for (int i = 0; i < kNumWeightBands; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        auto& b = p.bands[idx];
        b.on = wbandOn[idx]->load() > 0.5f;
        b.type = static_cast<int>(wbandType[idx]->load());
        b.freqHz = static_cast<double>(wbandFreq[idx]->load());
        b.gainDb = static_cast<double>(wbandGain[idx]->load());
        b.q = static_cast<double>(wbandQ[idx]->load());
    }
    p.rangeLoHz = static_cast<double>(rangeLoParam->load());
    p.rangeHiHz = static_cast<double>(rangeHiParam->load());

    spectral.setParams(p);
    spectral.setDetectionMode(static_cast<int>(detectionModeParam->load(std::memory_order_relaxed)));
    spectral.setExternalSidechainEnabled(externalSidechainParam->load(std::memory_order_relaxed) > .5f);

    const int ringSize = SpectralNode::kFftSize;
    using spectralmix::Mode;
    const Mode mode = bypassActive ? Mode::Bypass
                     : listenActive ? Mode::ScListen
                     : deltaActive ? Mode::Delta
                                   : Mode::Normal;
    monitorRoute.setMode(mode);
    float keyPeak = 0.0f;

    // Host block sizes may exceed the prepareToPlay hint. Keep every dry copy
    // within its preallocated storage and run the node on those same chunks.
    for (int offset = 0; offset < numSamples;) {
        const int count = juce::jmin(dryScratch.getNumSamples(), numSamples - offset);
        // Capture every physical key channel before any shared host-buffer write.
        for (int ch = 0; ch < 2; ++ch) {
            if (ch < keyChannels) {
                keyScratch.copyFrom(ch, 0, keyInput, ch, offset, count);
                for (int i = 0; i < count; ++i)
                    keyPeak = std::max(keyPeak, std::abs(keyScratch.getSample(ch, i)));
            } else {
                keyScratch.clear(ch, 0, count);
            }
        }
        std::array<float*, 2> wet{};
        for (int ch = 0; ch < numChannels; ++ch) {
            const auto channel = static_cast<std::size_t>(ch);
            auto& ring = dryRing[channel];
            wet[channel] = mainOutput.getWritePointer(ch, offset);
            float* dry = dryScratch.getWritePointer(ch);
            float* listen = listenScratch.getWritePointer(ch);
            const float* input = mainInput.getReadPointer(ch, offset);
            auto& keyRing = keyDelay[channel];
            int pos = dryRingWritePos;
            for (int i = 0; i < count; ++i) {
                dry[i] = ring[static_cast<std::size_t>(pos)];
                ring[static_cast<std::size_t>(pos)] = input[i];
                listen[i] = keyRing[static_cast<std::size_t>(pos)];
                const float rawKey = keyChannels == 0 ? 0.0f
                    : keyChannels == 1 ? keyScratch.getSample(0, i)
                    : numChannels == 1 ? .5f*(keyScratch.getSample(0, i)+keyScratch.getSample(1, i))
                    : keyScratch.getSample(ch, i);
                keyRing[static_cast<std::size_t>(pos)] = rawKey;
                wet[channel][i] = input[i];
                pos = (pos + 1) % ringSize;
            }
        }
        dryRingWritePos = (dryRingWritePos + count) % ringSize;

        // Keep overlap/add history warm during internal and host bypass.
        spectral.process(wet.data(), numChannels, count, keyScratch.getArrayOfReadPointers(), keyChannels);

        for (int i = 0; i < count; ++i) {
            const double mix = mixSmoother.getNextValue() / 100.0;
            const double outputGainLinear = std::pow(10.0, outputSmoother.getNextValue() / 20.0);
            const double wetGainLinear = std::pow(10.0, wetTrimSmoother.getNextValue() / 20.0);
            monitorRoute.advance();
            for (int ch = 0; ch < numChannels; ++ch) {
                const auto channel = static_cast<std::size_t>(ch);
                wet[channel][i] = static_cast<float>(monitorRoute.blend(
                    static_cast<double>(dryScratch.getSample(ch, i)),
                    static_cast<double>(wet[channel][i]),
                    static_cast<double>(listenScratch.getSample(ch, i)),
                    mix, outputGainLinear, wetGainLinear));
            }
        }
        offset += count;
    }

    // One bounded snapshot of the last hop, irrespective of host block size.
    spectral.copyReductionDb(reductionScratch.data(), SpectralNode::kNumBins);
    reduction.write(reductionScratch.data(), SpectralNode::kNumBins);
    sidechainLevelDb.store(keyPeak > 1e-5f ? 20.0f*std::log10(keyPeak) : -100.0f,
                           std::memory_order_relaxed);

    // Delta tap (post-blend): buffer now holds the delta signal (wet - dry);
    // publish THAT to the analyzer's spectrum FIFO instead of the pre-node
    // input, so the display matches what delta mode is actually auditioning.
    if (deltaActive || listenActive)
        specFifo.write(mainOutput.getArrayOfReadPointers(), numChannels, numSamples);

    // FIELD DIAGNOSTIC (temporary): publish this callback's duration as a percent
    // of the buffer time budget (numSamples / fs). One counter read + a few relaxed
    // stores; no allocation/locks/IO. REMOVE with the entry timestamp above.
    const std::uint64_t diagT1 = mach_absolute_time();
    const double diagFs = publishedSampleRate.load(std::memory_order_relaxed);
    if (diagFs > 0.0 && numSamples > 0) {
        const double elapsedNs = static_cast<double>(diagT1 - diagT0) * machTimebaseNsPerTick;
        const double budgetNs = (static_cast<double>(numSamples) / diagFs) * 1.0e9;
        const float pct = static_cast<float>(elapsedNs / budgetNs * 100.0);
        cbLastPct.store(pct, std::memory_order_relaxed);
        if (pct > cbMaxPct.load(std::memory_order_relaxed))
            cbMaxPct.store(pct, std::memory_order_relaxed);
        if (pct > cbMaxPctSinceOpen.load(std::memory_order_relaxed))
            cbMaxPctSinceOpen.store(pct, std::memory_order_relaxed);
    }
}

juce::AudioProcessorEditor* SpectralProcessor::createEditor() {
    return new SpectralEditor(*this);
}

void SpectralProcessor::getStateInformation(juce::MemoryBlock& destData) {
    const auto state = workflow.createProcessorState();
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void SpectralProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr) return;

    workflow.restoreProcessorState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new SpectralProcessor();
}
