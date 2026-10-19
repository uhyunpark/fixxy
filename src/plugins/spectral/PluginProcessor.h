#pragma once

#include <array>
#include <atomic>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "dsp/SpectralNode.h"
#include "dsp/SpectralWeight.h"
#include "plugins/ui/SpectrumFifo.h"
#include "plugins/spectral/ReductionPublisher.h"
#include "plugins/spectral/SpectralWorkflow.h"
#include "plugins/spectral/SpectralMix.h"

/**
    LpngSpectral — spectral dynamic-resonance suppressor (soothe-class) product.
    Thin plugin shell over the JUCE-free SpectralNode (src/dsp/): APVTS params
    -> smoothed Params -> node, with sample-wise smoothing at the output blend.
    The custom editor displays the input spectrum, reduction and weight curve.

    FIRST nonzero-latency product in the project: the node delays wet audio by
    exactly N=1024 samples (its STFT frame size), so the dry path is run
    through a matching N-sample ring-buffer delay before the dry/wet blend —
    see the ring buffer members below and processBlock()'s mix stage.
*/
class SpectralProcessor : public juce::AudioProcessor {
public:
    SpectralProcessor();

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

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

    // Declaration order is intentional: APVTS attachments use this manager,
    // and the processor-owned workflow must outlive every editor instance.
    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;
    SpectralWorkflow workflow;

    // Sample rate mirrored from prepareToPlay for the UI thread (trace
    // displays later) — mirrors the Comp/EQ publishedSampleRate pattern.
    std::atomic<double> publishedSampleRate{0.0};

    double getPublishedSampleRate() const {
        return publishedSampleRate.load(std::memory_order_relaxed);
    }

    // FIELD DIAGNOSTIC (temporary): audio-callback load readout. Each processBlock
    // publishes its wall-clock duration as a percentage of the buffer time budget
    // (numSamples / fs), so the editor can show whether the callback is overrunning
    // while audio crackles. REMOVE with the editor readout once diagnosed.
    //   cbLastPct         — this block's duration, % of budget.
    //   cbMaxPct          — running max, never reset (long-session peak).
    //   cbMaxPctSinceOpen — running max, reset to 0 from the editor constructor.
    std::atomic<float> cbLastPct{0.0f};
    std::atomic<float> cbMaxPct{0.0f};
    std::atomic<float> cbMaxPctSinceOpen{0.0f};

    // --- editor-facing publish surface (message-thread reads) ---
    // Input spectrum tap (pre-node), drained by the editor's 30 Hz analyzer.
    SpectrumFifo specFifo;
    // Latest per-bin reduction snapshot (dB, >= 0) for the editor's ember ribbon.
    ReductionPublisher<SpectralNode::kNumBins> reduction;
    std::atomic<float> sidechainLevelDb{-100.0f};

private:
    void process(juce::AudioBuffer<float>& buffer, bool hostBypassed);

    static constexpr int kAnalyzerFftSize = 4096;
    static constexpr double kAnalyzerMaxFs = 192000.0;
    static constexpr int kNumWeightBands = spectralweight::kMaxWeightBands;

    std::atomic<float>* depthParam = nullptr;
    std::atomic<float>* detailParam = nullptr;
    std::atomic<float>* attackParam = nullptr;
    std::atomic<float>* releaseParam = nullptr;
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* bypassParam = nullptr;
    std::atomic<float>* deltaParam = nullptr;

    // --- Sidebar globals (proto/spectral_sidebar.py). wetTrim is smoothed (a
    // continuous dB gesture applied in the output blend); the rest are block-
    // polled straight into Params like the weight bands — see processBlock(). ---
    std::atomic<float>* maxCutParam = nullptr;
    std::atomic<float>* wetTrimParam = nullptr;
    std::atomic<float>* msModeParam = nullptr;
    std::atomic<float>* linkPctParam = nullptr;
    std::atomic<float>* focusParam = nullptr;
    std::atomic<float>* tiltDetailParam = nullptr;
    std::atomic<float>* tiltAttackParam = nullptr;
    std::atomic<float>* tiltReleaseParam = nullptr;

    // --- Weight-curve bands (see dsp/SpectralWeight.h / SpectralNode::Params;
    // the node sums them into its per-bin weight table). Block-polled straight
    // into Params.bands[i] in processBlock; the node's per-bin attack/release
    // smooths their effect on attenuation. ---
    std::array<std::atomic<float>*, kNumWeightBands> wbandOn{};
    std::array<std::atomic<float>*, kNumWeightBands> wbandType{};
    std::array<std::atomic<float>*, kNumWeightBands> wbandFreq{};
    std::array<std::atomic<float>*, kNumWeightBands> wbandGain{};
    std::array<std::atomic<float>*, kNumWeightBands> wbandQ{};
    std::atomic<float>* rangeLoParam = nullptr;
    std::atomic<float>* rangeHiParam = nullptr;
    std::atomic<float>* detectionModeParam = nullptr;
    std::atomic<float>* externalSidechainParam = nullptr;
    std::atomic<float>* sidechainListenParam = nullptr;

    // Preallocated scratch for copying the node's per-bin reduction out to the
    // publisher once per block (no allocation on the audio thread).
    std::array<float, SpectralNode::kNumBins> reductionScratch{};

    juce::LinearSmoothedValue<double> depthSmoother;
    juce::LinearSmoothedValue<double> detailSmoother;
    juce::LinearSmoothedValue<double> attackSmoother;
    juce::LinearSmoothedValue<double> releaseSmoother;
    juce::LinearSmoothedValue<double> mixSmoother;
    juce::LinearSmoothedValue<double> outputSmoother;
    juce::LinearSmoothedValue<double> wetTrimSmoother;

    SpectralNode spectral;

    // FIELD DIAGNOSTIC (temporary): nanoseconds per mach_absolute_time() tick,
    // cached once in the constructor (mach_timebase_info is a syscall — never
    // called on the audio thread). REMOVE with the callback-load readout.
    double machTimebaseNsPerTick = 1.0;

    // Dry-delay ring: aligns the dry path to the node's fixed N=1024-sample
    // wet latency before the dry/wet blend. Allocated in prepareToPlay only;
    // read-then-write per sample in processBlock (no allocation there).
    std::array<std::vector<float>, 2> dryRing;
    int dryRingWritePos = 0;

    // Preallocated dry scratch, sized (numChannels, samplesPerBlock) in
    // prepareToPlay, reused in bounded chunks if a host sends a larger block.
    juce::AudioBuffer<float> dryScratch;
    juce::AudioBuffer<float> keyScratch;
    juce::AudioBuffer<float> listenScratch;
    std::array<std::vector<float>, 2> keyDelay;
    spectralmix::RouteCrossfade monitorRoute;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralProcessor)
};
