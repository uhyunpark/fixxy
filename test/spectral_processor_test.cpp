#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>
#include <CoreFoundation/CoreFoundation.h>

#include "plugins/spectral/PluginProcessor.h"
#include "plugins/spectral/ui/WeightBandLayer.h"
#include "plugins/spectral/SpectralMix.h"
#include "spectral_sidechain_fixtures.h"

static thread_local bool trackCallbackHeap = false;
static thread_local std::size_t callbackAllocations = 0, callbackDeletions = 0;
void* operator new(std::size_t n) {
    if (trackCallbackHeap) ++callbackAllocations;
    if (auto* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if (trackCallbackHeap) ++callbackDeletions; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 128;
int failures = 0;

void check(const char* name, double error, double tolerance) {
    const bool pass = error <= tolerance;
    std::printf("[%s] %s: max error %.9g (limit %.9g)\n",
                pass ? "PASS" : "FAIL", name, error, tolerance);
    if (!pass) ++failures;
}

void setParam(SpectralProcessor& processor, const char* id, float value) {
    auto* parameter = processor.apvts.getParameter(id);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

void fill(juce::AudioBuffer<float>& buffer, float value) {
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        std::fill_n(buffer.getWritePointer(ch), buffer.getNumSamples(), value);
}

void testRamp(const char* id) {
    auto processor = std::make_unique<SpectralProcessor>();
    setParam(*processor, "depth", 0.0f);
    const bool mix = juce::String(id) == "mix";
    if (mix) {
        setParam(*processor, "mix", 0.0f);
        setParam(*processor, "wetTrim", -6.0f);
    }
    processor->prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    for (int n = 0; n < 2048; n += blockSize) {
        fill(buffer, 0.25f);
        processor->processBlock(buffer, midi);
    }
    setParam(*processor, id, mix ? 100.0f : -6.0f);
    double error = 0.0;
    for (int n = 0; n < 1024; n += blockSize) {
        fill(buffer, 0.25f);
        processor->processBlock(buffer, midi);
        for (int i = 0; i < blockSize; ++i) {
            const double progress = std::min(1.0, static_cast<double>(n + i + 1) / 960.0);
            const double expected = 0.25 * (mix
                ? 1.0 + progress * (std::pow(10.0, -6.0 / 20.0) - 1.0)
                : std::pow(10.0, -6.0 * progress / 20.0));
            for (int ch = 0; ch < 2; ++ch) {
                const double actual = buffer.getSample(ch, i);
                if (!std::isfinite(actual)) {
                    error = 1.0;
                    continue;
                }
                error = std::max(error, std::abs(actual - expected));
            }
        }
    }
    check(id, error, 3.0e-8);
}

void testHostBypass() {
    auto processor = std::make_unique<SpectralProcessor>();
    processor->prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    double error = 0.0;
    for (int n = 0; n < 2048; n += blockSize) {
        buffer.clear();
        if (n == 0) buffer.setSample(0, 0, 0.5f);
        processor->processBlockBypassed(buffer, midi);
        for (int i = 0; i < blockSize; ++i) {
            const double expected = n + i == processor->getLatencySamples() ? 0.5 : 0.0;
            const double actual = buffer.getSample(0, i);
            if (!std::isfinite(actual)) {
                error = 1.0;
                continue;
            }
            error = std::max(error, std::abs(actual - expected));
        }
    }
    check("host bypass preserves reported latency", error, 0.0);
}

std::vector<float> render(int preparedSize, int hostSize, bool bypass) {
    auto processor = std::make_unique<SpectralProcessor>();
    setParam(*processor, "mix", 37.0f);
    processor->prepareToPlay(sampleRate, preparedSize);
    constexpr int length = 16384;
    std::vector<float> result(static_cast<std::size_t>(length * 2));
    juce::MidiBuffer midi;
    for (int offset = 0; offset < length;) {
        const int count = std::min(hostSize, length - offset);
        juce::AudioBuffer<float> buffer(2, count);
        for (int i = 0; i < count; ++i) {
            const double phase = static_cast<double>(offset + i) / sampleRate;
            buffer.setSample(0, i, static_cast<float>(0.2 * std::sin(phase * 3000.0 * juce::MathConstants<double>::twoPi)));
            buffer.setSample(1, i, static_cast<float>(0.1 * std::cos(phase * 750.0 * juce::MathConstants<double>::twoPi)));
        }
        if (bypass) processor->processBlockBypassed(buffer, midi);
        else processor->processBlock(buffer, midi);
        for (int ch = 0; ch < 2; ++ch)
            std::copy_n(buffer.getReadPointer(ch), count,
                        result.begin() + ch * length + offset);
        offset += count;
    }
    return result;
}

void testOversizedBlock(bool bypass) {
    const auto reference = render(128, 128, bypass);
    const auto oversized = render(128, 8193, bypass);
    double error = 0.0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        if (!std::isfinite(oversized[i])) { error = 1.0; break; }
        error = std::max(error, std::abs(static_cast<double>(oversized[i]) - reference[i]));
    }
    check(bypass ? "oversized host bypass" : "oversized block matches regular processing", error, 0.0);
}

std::vector<float> renderDetectionMode(int mode, bool recall, bool liveSwitch = false) {
    auto processor = std::make_unique<SpectralProcessor>();
    setParam(*processor, "detectionMode", liveSwitch ? 0.0f : static_cast<float>(mode));
    if (recall) {
        juce::MemoryBlock state;
        processor->getStateInformation(state);
        processor = std::make_unique<SpectralProcessor>();
        processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    }
    processor->prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> result(16384);
    for (int at = 0; at < static_cast<int>(result.size()); at += blockSize) {
        if (liveSwitch && at == 8192) setParam(*processor, "detectionMode", 1.0f);
        for (int i = 0; i < blockSize; ++i) {
            const double t = static_cast<double>(at+i)/sampleRate;
            const auto value = static_cast<float>(.3*std::sin(juce::MathConstants<double>::twoPi*3000*t)
                                                 +.07*std::sin(juce::MathConstants<double>::twoPi*731*t));
            buffer.setSample(0, i, value); buffer.setSample(1, i, value);
        }
        processor->processBlock(buffer, midi);
        std::copy_n(buffer.getReadPointer(0), blockSize, result.data()+at);
    }
    check("detection mode retains reported latency", std::abs(processor->getLatencySamples()-1024), 0.0);
    return result;
}

void testDetectionModeAudioAndRecall() {
    const auto soft = renderDetectionMode(0, false);
    const auto hard = renderDetectionMode(1, false);
    const auto recalled = renderDetectionMode(1, true);
    const auto switched = renderDetectionMode(1, false, true);
    double difference = 0.0, recallError = 0.0, prefixError = 0.0, switchedDifference = 0.0;
    for (std::size_t i = 0; i < soft.size(); ++i) {
        difference = std::max(difference, std::abs(static_cast<double>(soft[i])-hard[i]));
        recallError = std::max(recallError, std::abs(static_cast<double>(recalled[i])-hard[i]));
        if (i < 8192) prefixError = std::max(prefixError, std::abs(static_cast<double>(switched[i])-soft[i]));
        else switchedDifference = std::max(switchedDifference, std::abs(static_cast<double>(switched[i])-soft[i]));
    }
    check("APVTS Hard changes actual processor audio", difference > .01 ? 0.0 : 1.0, 0.0);
    check("Hard state recalls exact processed audio", recallError, 0.0);
    check("live mode automation preserves earlier Soft output", prefixError, 0.0);
    check("live mode automation changes subsequent audio", switchedDifference > .01 ? 0.0 : 1.0, 0.0);
}

void testSidechainLayoutsAndAudio() {
    namespace fx = spectralsidechainfix;
    auto processor = std::make_unique<SpectralProcessor>();
    check("optional sidechain bus exists", processor->getBusCount(true) == 2 ? 0 : 1, 0);
    for (int mainChannels : {1, 2}) {
        for (int keyChannels : {0, 1, 2, 3}) {
            auto layout = processor->getBusesLayout();
            layout.inputBuses.set(0, juce::AudioChannelSet::canonicalChannelSet(mainChannels));
            layout.outputBuses.set(0, juce::AudioChannelSet::canonicalChannelSet(mainChannels));
            layout.inputBuses.set(1, juce::AudioChannelSet::canonicalChannelSet(keyChannels));
            check("main mono/stereo with absent/mono/stereo key only",
                  processor->isBusesLayoutSupported(layout) == (keyChannels <= 2) ? 0 : 1, 0);
        }
    }
    double worst = 0.0;
    for (int scenario = 0; scenario < 5; ++scenario) {
        const int mainChannels = scenario == 3 ? 1 : 2;
        const int keyChannels = scenario == 2 ? 1 : scenario == 4 ? 0 : 2;
        auto layout = processor->getBusesLayout();
        layout.inputBuses.set(0, juce::AudioChannelSet::canonicalChannelSet(mainChannels));
        layout.outputBuses.set(0, juce::AudioChannelSet::canonicalChannelSet(mainChannels));
        layout.inputBuses.set(1, juce::AudioChannelSet::canonicalChannelSet(keyChannels));
        check("sidechain layout applies", processor->setBusesLayout(layout) ? 0 : 1, 0);
        setParam(*processor, "externalSidechain", 1);
        setParam(*processor, "detectionMode", scenario >= 1 && scenario <= 3 ? 1 : 0);
        setParam(*processor, "msMode", scenario == 2 ? 1 : 0);
        setParam(*processor, "linkPct", scenario == 2 ? 0 : 100);
        processor->prepareToPlay(sampleRate, 97);
        const std::array<const double*, 5> expected{fx::kSoft.data(), fx::kHard.data(),
            fx::kMonoKeyMs.data(), fx::kStereoKeyMono.data(), fx::kMissing.data()};
        juce::MidiBuffer midi;
        for (int offset = 0; offset < static_cast<int>(fx::kOutputLength);) {
            const int count = std::min(479, static_cast<int>(fx::kOutputLength)-offset);
            juce::AudioBuffer<float> buffer(mainChannels+keyChannels, count);
            buffer.clear();
            for (int i = 0; i < count; ++i) {
                const auto n = static_cast<std::size_t>(offset+i);
                if (n >= fx::kInputLength) continue;
                for (int ch = 0; ch < mainChannels; ++ch)
                    buffer.setSample(ch, i, static_cast<float>(fx::kMain[static_cast<std::size_t>(ch)*fx::kInputLength+n]));
                for (int ch = 0; ch < keyChannels; ++ch)
                    buffer.setSample(mainChannels+ch, i, static_cast<float>(fx::kKey[static_cast<std::size_t>(ch)*fx::kInputLength+n]));
            }
            trackCallbackHeap = true;
            processor->processBlock(buffer, midi);
            trackCallbackHeap = false;
            for (int ch = 0; ch < mainChannels; ++ch)
                for (int i = 0; i < count; ++i)
                    worst = std::max(worst, std::abs(buffer.getSample(ch, i)-expected[static_cast<std::size_t>(scenario)][static_cast<std::size_t>(ch)*fx::kOutputLength+static_cast<std::size_t>(offset+i)]));
            offset += count;
        }
        check("sidechain keeps N latency", std::abs(processor->getLatencySamples()-1024), 0);
    }
    check("processor key bus matches five proto audio fixtures", worst, 1e-7);
}

void testMonitorCrossfade() {
    namespace fx = spectralsidechainfix;
    spectralmix::RouteCrossfade route;
    route.prepare(sampleRate, spectralmix::Mode::Normal);
    double worst = 0.0;
    for (std::size_t n = 0; n < fx::kOutputLength; ++n) {
        const auto mode = n < 1050 ? spectralmix::Mode::Normal
            : n < 1150 ? spectralmix::Mode::ScListen
            : n < 1500 ? spectralmix::Mode::Bypass
            : n < 1900 ? spectralmix::Mode::Delta : spectralmix::Mode::Normal;
        route.setMode(mode);
        route.advance();
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const double dry = n < fx::kLatency ? 0.0 : fx::kMain[ch*fx::kInputLength+n-fx::kLatency];
            const double result = route.blend(dry, fx::kSwitch[ch*fx::kOutputLength+n],
                fx::kAlignedKey[ch*fx::kOutputLength+n], fx::kMonitorMix, fx::kMonitorOutGain, fx::kMonitorWetGain);
            worst = std::max(worst, std::abs(result-fx::kMonitor[ch*fx::kOutputLength+n]));
        }
    }
    check("monitor 5ms weights match interrupted-fade independent oracle", worst, 1e-12);
}

void testRawKeyMonitor() {
    // Monitoring follows physical key routing and remains independent of the
    // external-detection switch, M/S, mix and wet trim. All buffers are prepared.
    double worst = 0.0;
    for (int mainChannels : {1, 2}) for (int keyChannels : {0, 1, 2}) {
        auto processor = std::make_unique<SpectralProcessor>();
        auto layout = processor->getBusesLayout();
        layout.inputBuses.set(0, juce::AudioChannelSet::canonicalChannelSet(mainChannels));
        layout.outputBuses.set(0, juce::AudioChannelSet::canonicalChannelSet(mainChannels));
        layout.inputBuses.set(1, juce::AudioChannelSet::canonicalChannelSet(keyChannels));
        processor->setBusesLayout(layout);
        setParam(*processor, "sidechainListen", 1);
        setParam(*processor, "delta", 1);
        setParam(*processor, "msMode", 1);
        setParam(*processor, "mix", 0);
        setParam(*processor, "wetTrim", 12);
        setParam(*processor, "output", -6);
        processor->prepareToPlay(sampleRate, 97);
        juce::MidiBuffer midi;
        constexpr int length = 3584;
        std::array<std::array<float, length>, 2> keys{};
        for (int n = 0; n < length; ++n) {
            keys[0][static_cast<std::size_t>(n)] = static_cast<float>(.2*std::sin(.071*n));
            keys[1][static_cast<std::size_t>(n)] = static_cast<float>(.1*std::cos(.043*n));
        }
        double expectedPeak = 0.0;
        for (int offset = 0; offset < length; offset += 256) {
            juce::AudioBuffer<float> buffer(mainChannels+keyChannels, 256);
            fill(buffer, .3f);
            expectedPeak = 0.0;
            for (int ch = 0; ch < keyChannels; ++ch)
                for (int i = 0; i < 256; ++i) {
                    const float value = keys[static_cast<std::size_t>(ch)][static_cast<std::size_t>(offset+i)];
                    buffer.setSample(mainChannels+ch, i, value);
                    expectedPeak = std::max(expectedPeak, static_cast<double>(std::abs(value)));
                }
            trackCallbackHeap = true;
            processor->processBlock(buffer, midi);
            trackCallbackHeap = false;
            for (int ch = 0; ch < mainChannels; ++ch)
                for (int i = 0; i < 256; ++i) {
                    const int source = offset+i-1024;
                    double expected = 0.0;
                    if (source >= 0 && keyChannels > 0) {
                        const auto n = static_cast<std::size_t>(source);
                        expected = keyChannels == 1 ? keys[0][n]
                            : mainChannels == 1 ? .5*(keys[0][n]+keys[1][n])
                            : keys[static_cast<std::size_t>(ch)][n];
                    }
                    expected *= std::pow(10.0, -6.0/20.0);
                    worst = std::max(worst, std::abs(buffer.getSample(ch, i)-expected));
                }
        }
        check("SC meter publishes physical key peak while detection disabled",
              std::abs(processor->sidechainLevelDb.load()-(expectedPeak > 1e-5 ? 20.0*std::log10(expectedPeak) : -100.0)), 2e-6);
        // The bypass crossfade settles during the first chunk. Its endpoint
        // must ignore listen, delta, output trim and wet trim.
        setParam(*processor, "bypass", 1);
        juce::AudioBuffer<float> buffer(mainChannels+keyChannels, 256);
        fill(buffer, .3f);
        processor->processBlock(buffer, midi);
        check("bypass overrides SC listen and gain controls after 5ms",
              std::abs(buffer.getSample(0, 255)-.3f), 0);
    }
    check("physical raw-key monitor/downmix/latency/precedence", worst, 2e-8);
    check("prepared processor callbacks have no new/delete", static_cast<double>(callbackAllocations+callbackDeletions), 0);
}

void testProcessorMonitorSchedule() {
    namespace fx = spectralsidechainfix;
    auto processor = std::make_unique<SpectralProcessor>();
    setParam(*processor, "mix", 37);
    setParam(*processor, "output", static_cast<float>(20*std::log10(fx::kMonitorOutGain)));
    setParam(*processor, "wetTrim", static_cast<float>(20*std::log10(fx::kMonitorWetGain)));
    processor->prepareToPlay(sampleRate, 97);
    // The host schema rounds gain controls to 0.1 dB. Correct only this
    // quantization relative to the verified fixture's exact .8/1.4 gains.
    const double actualOutput = std::pow(10.0, processor->apvts.getRawParameterValue("output")->load()/20.0);
    const double actualWet = std::pow(10.0, processor->apvts.getRawParameterValue("wetTrim")->load()/20.0);
    juce::MidiBuffer midi;
    double worst = 0.0;
    for (int offset = 0; offset < static_cast<int>(fx::kOutputLength);) {
        int count = std::min(479, static_cast<int>(fx::kOutputLength)-offset);
        for (int event : {512, 768, 1024, 1050, 1150, 1280, 1500, 1900})
            if (event > offset) count = std::min(count, event-offset);
        const int keyChannels = offset < 512 ? 2 : offset < 768 ? 0 : offset < 1024 ? 1 : 2;
        auto layout = processor->getBusesLayout();
        layout.inputBuses.set(1, juce::AudioChannelSet::canonicalChannelSet(keyChannels));
        processor->setBusesLayout(layout);
        setParam(*processor, "externalSidechain", offset >= 512 && offset < 1024 ? 1 : 0);
        setParam(*processor, "detectionMode", offset >= 512 && offset < 1024 ? 1 : 0);
        setParam(*processor, "sidechainListen", offset >= 1050 && offset < 1500 ? 1 : 0);
        setParam(*processor, "bypass", offset >= 1150 && offset < 1500 ? 1 : 0);
        setParam(*processor, "delta", offset >= 1050 && offset < 1900 ? 1 : 0);
        juce::AudioBuffer<float> buffer(2+keyChannels, count);
        buffer.clear();
        for (int i = 0; i < count; ++i) {
            const auto n = static_cast<std::size_t>(offset+i);
            if (n >= fx::kInputLength) continue;
            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample(ch, i, static_cast<float>(fx::kMain[static_cast<std::size_t>(ch)*fx::kInputLength+n]));
            for (int ch = 0; ch < keyChannels; ++ch)
                buffer.setSample(2+ch, i, static_cast<float>(fx::kKey[static_cast<std::size_t>(ch)*fx::kInputLength+n]));
        }
        trackCallbackHeap = true;
        processor->processBlock(buffer, midi);
        trackCallbackHeap = false;
        for (int i = 0; i < count; ++i) {
            const int n = offset+i;
            double normal = 0, delta = 0, listen = 0;
            // Closed-form weights for the fixture's event times; the 100/240
            // interrupted SC fade starts the bypass ramp at 5/12 SC + 7/12 N.
            if (n < 1050) normal = 1;
            else if (n < 1150) { listen = (n-1049)/240.0; normal = 1-listen; }
            else if (n < 1390) { normal = (7.0/12)*(1-(n-1149)/240.0); listen = (5.0/12)*(1-(n-1149)/240.0); }
            else if (n < 1500) {}
            else if (n < 1740) delta = (n-1499)/240.0;
            else if (n < 1900) delta = 1;
            else if (n < 2140) { normal = (n-1899)/240.0; delta = 1-normal; }
            else normal = 1;
            for (std::size_t ch = 0; ch < 2; ++ch) {
                const auto index = ch*fx::kOutputLength+static_cast<std::size_t>(n);
                const double dry = n < fx::kLatency ? 0 : fx::kMain[ch*fx::kInputLength+static_cast<std::size_t>(n-fx::kLatency)];
                const double wet = fx::kSwitch[index];
                const double correction = normal*((.63*dry+.37*wet*actualWet)*actualOutput-(.63*dry+.37*wet*1.4)*.8)
                    + delta*((wet*actualWet-dry)*actualOutput-(wet*1.4-dry)*.8)
                    + listen*fx::kAlignedKey[index]*(actualOutput-.8);
                worst = std::max(worst, std::abs(buffer.getSample(static_cast<int>(ch), i)-(fx::kMonitor[index]+correction)));
            }
        }
        offset += count;
    }
    check("complete processor interrupted monitor + source/mode/connection schedule", worst, 1e-7);
    check("monitor changes and bus count changes allocate/delete nothing in callback", static_cast<double>(callbackAllocations+callbackDeletions), 0);
}

void benchmarkHardCallbacks() {
    for (double rate : {48000.0, 96000.0, 192000.0}) {
        for (int count : {64, 256, 1024}) {
            SpectralProcessor processor;
            setParam(processor, "detectionMode", 1.0f);
            setParam(processor, "msMode", 1.0f);
            setParam(processor, "linkPct", 50.0f);
            setParam(processor, "focus", .3f);
            processor.prepareToPlay(rate, count);
            juce::AudioBuffer<float> buffer(2, count);
            juce::MidiBuffer midi;
            std::array<double, 200> times{};
            for (int block = -32; block < 200; ++block) {
                for (int i = 0; i < count; ++i) {
                    const double phase = static_cast<double>((block+32)*count+i)/rate;
                    buffer.setSample(0, i, static_cast<float>(.3*std::sin(juce::MathConstants<double>::twoPi*3000*phase)));
                    buffer.setSample(1, i, static_cast<float>(.2*std::sin(juce::MathConstants<double>::twoPi*3173*phase)));
                }
                const auto start = std::chrono::steady_clock::now();
                processor.processBlock(buffer, midi);
                if (block >= 0) times[static_cast<std::size_t>(block)] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()-start).count();
            }
            double sum = 0; for (double time : times) sum += time;
            std::sort(times.begin(), times.end());
            std::printf("Hard callback fs=%.0f block=%d mean=%.3f median=%.3f p99=%.3f max=%.3f us deadline=%.3f us\n",
                        rate, count, sum/times.size(), .5*(times[99]+times[100]), times[197], times[199], 1e6*count/rate);
        }
    }
}

void captureEditor(const juce::File& directory) {
    auto processor = std::make_unique<SpectralProcessor>();
    setParam(*processor, "wband0On", 1.0f);
    setParam(*processor, "wband0Freq", 3000.0f);
    setParam(*processor, "wband0Gain", 6.0f);
    setParam(*processor, "rangeLo", 80.0f);
    setParam(*processor, "rangeHi", 16000.0f);
    processor->prepareToPlay(sampleRate, blockSize);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditor());
    editor->setVisible(true);
    WeightBandLayer* layer = nullptr;
    for (auto* child : editor->getChildren())
        if (auto* candidate = dynamic_cast<WeightBandLayer*>(child)) layer = candidate;
    if (layer == nullptr || directory.createDirectory().failed()) {
        check("native editor capture setup", 1.0, 0.0);
        return;
    }
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    juce::Random random(42);
    int sample = 0;
    const auto feedSignal = [&] {
        for (int frame = 0; frame < 50; ++frame) {
            for (int block = 0; block < 6; ++block) {
                for (int i = 0; i < blockSize; ++i, ++sample) {
                    const double phase = static_cast<double>(sample) / sampleRate;
                    const float value = static_cast<float>(0.22 * std::sin(phase * 3000.0 * juce::MathConstants<double>::twoPi)
                        + 0.07 * std::sin(phase * 750.0 * juce::MathConstants<double>::twoPi)
                        + 0.04 * (random.nextDouble() * 2.0 - 1.0));
                    buffer.setSample(0, i, value);
                    buffer.setSample(1, i, value * 0.8f);
                    if (buffer.getNumChannels() == 4) {
                        buffer.setSample(2, i, static_cast<float>(.35*std::sin(phase*3000*juce::MathConstants<double>::twoPi)));
                        buffer.setSample(3, i, static_cast<float>(.2*std::sin(phase*750*juce::MathConstants<double>::twoPi)));
                    }
                }
                processor->processBlock(buffer, midi);
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
            juce::Timer::callPendingTimersSynchronously();
        }
    };
    const auto save = [&](const juce::String& name) {
        const auto snapshot = editor->createComponentSnapshot(editor->getLocalBounds());
        juce::FileOutputStream output(directory.getChildFile(name + ".png"));
        const bool written = output.openedOk() && output.setPosition(0)
            && output.truncate().wasOk() && juce::PNGImageFormat().writeImageToStream(snapshot, output);
        check(name.toRawUTF8(), written ? 0.0 : 1.0, 0.0);
    };
    for (const auto size : {juce::Point<int>{892, 520}, {1132, 640}, {1600, 900}}) {
        editor->setSize(size.x, size.y);
        spectralui::DisplayMap map;
        map.setBounds(6.0f, 6.0f, static_cast<float>(layer->getWidth() - 12),
                      static_cast<float>(layer->getHeight() - 12));
        const juce::Point<float> point{map.freqToX(3000.0f), map.weightDbToY(6.0f)};
        const auto now = juce::Time::getCurrentTime();
        juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), point,
            juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            layer, layer, now, point, now, 1, false);
        layer->mouseDown(event);
        layer->mouseUp(event);
        bool hasBandControls = false;
        for (auto* child : editor->getChildren())
            if (dynamic_cast<juce::ComboBox*>(child) != nullptr && child->isVisible()) hasBandControls = true;
        check("click selects a visible band editor", hasBandControls ? 0.0 : 1.0, 0.0);
        feedSignal();
        save("spectral-" + juce::String(size.x) + "x" + juce::String(size.y));
    }
    editor->setSize(1132, 640);
    setParam(*processor, "delta", 1.0f);
    feedSignal();
    save("spectral-delta");
    layer->clearSelection();
    setParam(*processor, "delta", 0.0f);
    feedSignal();
    save("spectral-unselected");

    // Workflow presentation states: a bypassed band remains present as a
    // hollow selectable node, then the details rail and context help can be
    // inspected independently in stable native snapshots.
    setParam(*processor, "wband0On", 0.0f);
    feedSignal();
    save("spectral-band-bypassed");

    const auto clickTextButton = [&](const juce::String& text) {
        for (auto* child : editor->getChildren()) {
            if (auto* button = dynamic_cast<juce::TextButton*>(child);
                button != nullptr && button->getButtonText() == text) {
                button->onClick();
                return true;
            }
        }
        return false;
    };
    check("details collapse button is available", clickTextButton("details <") ? 0.0 : 1.0, 0.0);
    save("spectral-details-collapsed");
    check("context help button is available", clickTextButton("?") ? 0.0 : 1.0, 0.0);
    save("spectral-context-help");
    clickTextButton("?");

    // Restore only workflow metadata while keeping the same audible parameter
    // values, with the editor already open and its analyzer receiving signal.
    processor->workflow.copyActiveToOther();
    processor->workflow.selectSlot(SpectralWorkflow::Slot::b);
    check("host-readback snapshot preset saves",
          processor->workflow.savePreset(directory.getChildFile("host-readback.fixxyspectral")).wasOk() ? 0.0 : 1.0,
          0.0);
    juce::MemoryBlock collapsedState;
    processor->getStateInformation(collapsedState);
    processor->workflow.selectSlot(SpectralWorkflow::Slot::a);
    processor->workflow.setSidebarExpanded(true);
    juce::MemoryBlock expandedState;
    processor->getStateInformation(expandedState);
    feedSignal();
    processor->setStateInformation(collapsedState.getData(), static_cast<int>(collapsedState.getSize()));
    feedSignal();
    save("spectral-host-restored-collapsed");
    processor->setStateInformation(expandedState.getData(), static_cast<int>(expandedState.getSize()));
    feedSignal();
    save("spectral-host-restored-expanded");

    setParam(*processor, "detectionMode", 1.0f);
    feedSignal();
    auto* tilt = editor->findChildWithID("tiltDetail");
    auto* detail = editor->findChildWithID("detail");
    check("Hard disables only Detail tilt, keeps main Detail active",
          tilt != nullptr && !tilt->isEnabled() && detail != nullptr && detail->isEnabled() ? 0.0 : 1.0, 0.0);
    save("spectral-hard");
    editor->setSize(892, 520);
    feedSignal();
    save("spectral-hard-minimum");
    clickTextButton("details <");
    feedSignal();
    save("spectral-hard-collapsed");
    juce::MemoryBlock hardState;
    processor->getStateInformation(hardState);
    setParam(*processor, "detectionMode", 0.0f);
    feedSignal();
    check("Soft host change re-enables Detail tilt", tilt != nullptr && tilt->isEnabled() ? 0.0 : 1.0, 0.0);
    processor->setStateInformation(hardState.getData(), static_cast<int>(hardState.getSize()));
    feedSignal();
    check("Hard host restore refreshes Detail tilt", tilt != nullptr && !tilt->isEnabled() ? 0.0 : 1.0, 0.0);
    save("spectral-hard-restored");

    auto sidechainLayout = processor->getBusesLayout();
    sidechainLayout.inputBuses.set(1, juce::AudioChannelSet::stereo());
    check("native sidechain stereo bus connects", processor->setBusesLayout(sidechainLayout) ? 0 : 1, 0);
    buffer.setSize(4, blockSize);
    setParam(*processor, "externalSidechain", 1);
    setParam(*processor, "sidechainListen", 1);
    processor->prepareToPlay(sampleRate, blockSize);
    processor->workflow.setSidebarExpanded(true);
    feedSignal();
    check("native sidechain meter receives real key signal", processor->sidechainLevelDb.load() > -20 ? 0 : 1, 0);
    save("spectral-sidechain-minimum");
    processor->workflow.setSidebarExpanded(false);
    feedSignal();
    save("spectral-sidechain-collapsed");
    setParam(*processor, "sidechainListen", 0);
    feedSignal();
    save("spectral-sidechain-detection");
}
}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juce;
    if (argc > 1 && juce::String(argv[1]) == "--hard-benchmark") { benchmarkHardCallbacks(); return 0; }
    testRamp("output");
    testRamp("wetTrim");
    testRamp("mix");
    testHostBypass();
    testOversizedBlock(false);
    testOversizedBlock(true);
    testDetectionModeAudioAndRecall();
    testSidechainLayoutsAndAudio();
    testMonitorCrossfade();
    testRawKeyMonitor();
    testProcessorMonitorSchedule();
    if (argc == 3 && juce::String(argv[1]) == "--snapshots")
        captureEditor(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]));
    return failures == 0 ? 0 : 1;
}
