// Verified proto bridge. Private state is exposed only in this isolated test TU
// to check retained histories and both lanes without expanding the public API.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
#include "dsp/SpectralFft.h"
#include "dsp/SpectralWeight.h"
#include "dsp/SpectralControl.h"
#define private public
#include "dsp/SpectralNode.h"
#undef private
#include "spectral_cap_fixtures.h"

static bool trackHeap = false;
static std::size_t allocations = 0, deletions = 0;
void* operator new(std::size_t n) {
    if (trackHeap) ++allocations;
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if (trackHeap) ++deletions; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
namespace fx = spectralcapfix;
using Audio = std::array<std::vector<float>, 2>;
bool passed = true;
void check(const char* name, bool ok, double error = 0) {
    std::printf("%s %s (%.12g)\n", ok ? "PASS" : "FAIL", name, error);
    passed &= ok;
}

Audio render(int mode, bool ms, int chunk, bool inspect, double& audioError,
             double& reductionError, double& boundError) {
    constexpr int total = fx::kInputLength + fx::kFftSize;
    Audio audio;
    for (auto& lane : audio) lane.resize(total);
    for (int n = 0; n < fx::kInputLength; ++n) {
        const double t = static_cast<double>(n)/fx::kSampleRate;
        const double phase = 2*3.14159265358979323846*t;
        audio[0][static_cast<std::size_t>(n)] = static_cast<float>(.30*std::sin(7500*phase)+.03*std::sin(731*phase));
        audio[1][static_cast<std::size_t>(n)] = static_cast<float>(.17*std::sin(9000*phase)-.05*std::sin(731*phase));
    }
    SpectralNode node;
    SpectralNode::Params p;
    p.depth = 10; p.detail = 10; p.attackMs = 1; p.releaseMs = 1000;
    p.tiltDetail = 1; p.linkPct = 0; p.msMode = ms;
    node.setParams(p);
    node.prepare(fx::kSampleRate, 97, 2);
    node.setDetectionMode(mode);
    const auto& expectedAudio = ms ? (mode ? fx::kMsHardAudio : fx::kMsSoftAudio)
                                   : (mode ? fx::kLrHardAudio : fx::kLrSoftAudio);
    const auto& expectedRed = ms ? (mode ? fx::kMsHardReduction : fx::kMsSoftReduction)
                                 : (mode ? fx::kLrHardReduction : fx::kLrSoftReduction);
    double cap = fx::kLegacyBound;
    bool reflected = true;
    const double alpha = std::exp(-256/(fx::kSampleRate*.020));
    std::size_t event = 0;
    for (int pos = 0; pos < total;) {
        if (event < fx::kEventSamples.size() && pos == fx::kEventSamples[event]) {
            p.maxCutDb = fx::kEventCaps[event++];
            trackHeap = true; node.setParams(p); trackHeap = false;
        }
        int count = std::min(chunk, total-pos);
        if (event < fx::kEventSamples.size()) count = std::min(count, fx::kEventSamples[event]-pos);
        if (inspect) count = std::min(count, fx::kHop-pos%fx::kHop);
        float* channels[]{audio[0].data()+pos, audio[1].data()+pos};
        trackHeap = true; node.process(channels, 2, count); trackHeap = false;
        pos += count;
        if (inspect && pos%fx::kHop == 0) {
            const double target = p.maxCutDb >= 24 ? fx::kLegacyBound : p.maxCutDb;
            cap = alpha*cap+(1-alpha)*target;
            if (std::abs(cap-target) <= 1e-6) cap = target;
            boundError = std::max(boundError, std::abs(node.effectiveCapDb_-cap));
            for (const auto& lane : node.ballY1_)
                for (double y : lane) boundError = std::max(boundError, std::max(-y, y-cap));
            for (std::size_t b = 0; b < node.redDb_.size(); ++b) {
                reflected &= node.redA_[b] == node.ballY1_[0][b] && node.redB_[b] == node.ballY1_[1][b];
                reflected &= node.gainA_[b] == std::pow(10.0, -node.redA_[b]/20.0)
                          && node.gainB_[b] == std::pow(10.0, -node.redB_[b]/20.0);
                boundError = std::max(boundError, std::pow(10.0, -cap/20.0)-std::min(node.gainA_[b], node.gainB_[b]));
            }
            const int frame = pos/fx::kHop-1;
            if (mode == 0 && frame == 30)
                check("stable Off preserves legitimate Soft reduction above 40 dB",
                    node.finalClampBypass_ && *std::max_element(node.ballY1_[0].begin(), node.ballY1_[0].end()) > 40);
            for (std::size_t i = 0; i < fx::kProbeFrames.size(); ++i)
                if (frame == fx::kProbeFrames[i])
                    for (std::size_t lane = 0; lane < 2; ++lane)
                        for (std::size_t b = 0; b < fx::kProbeBins.size(); ++b) {
                            const auto bin = static_cast<std::size_t>(fx::kProbeBins[b]);
                            reductionError = std::max(reductionError, std::abs(node.ballY1_[lane][bin]-expectedRed[(i*2+lane)*3+b]));
                        }
        }
    }
    if (inspect) check("gain and lane publication use clamped stored y", reflected);
    for (std::size_t lane = 0; lane < 2; ++lane)
        for (std::size_t i = 0; i < fx::kProbeSamples.size(); ++i)
            audioError = std::max(audioError, std::abs(audio[lane][static_cast<std::size_t>(fx::kProbeSamples[i])]-expectedAudio[lane*fx::kProbeSamples.size()+i]));
    return audio;
}

void scalarHistory() {
    SpectralNode node;
    node.prepare(48000, 256, 2);
    node.weightDb_.fill(0);
    node.aAArr_.fill(.9); node.aRArr_.fill(.999);
    node.ballY1_[0].fill(110); node.ballS1_[0].fill(120);
    std::array<double, SpectralNode::kNumBins> magnitude{}, maximum{}, gain{}, red{};
    magnitude.fill(1);
    maximum.fill(100/(.5*(1+std::tanh(-SpectralNode::kKneeC/SpectralNode::kKneeW))));
    double error = 0;
    bool snapped = false;
    for (std::size_t row = 0; row < fx::kScalarRows.size(); row += 6) {
        node.maxCutDb_ = fx::kScalarRows[row];
        node.advanceMaxCut();
        trackHeap = true;
        node.computeGainImpl(magnitude.data(), maximum.data(), 0, true, gain.data(), red.data());
        trackHeap = false;
        const std::array<double, 5> actual{node.effectiveCapDb_, node.ballY1_[0][0],
            node.ballS1_[0][0], gain[0], node.finalClampBypass_ ? 1.0 : 0.0};
        for (std::size_t j = 0; j < actual.size(); ++j)
            error = std::max(error, std::abs(actual[j]-fx::kScalarRows[row+j+1]));
        snapped |= node.finalClampBypass_;
    }
    check("112 scalar fixture rows: cap/y/s/gain/bypass", error < 1e-10, error);
    check("Off finite completion", snapped);
    const auto previousS = node.ballS1_[0], previousY = node.ballY1_[0];
    node.computeGainImpl(magnitude.data(), maximum.data(), 0, false, gain.data(), red.data());
    check("inactive lane preserves both histories", previousS == node.ballS1_[0] && previousY == node.ballY1_[0]);
    check("inactive lane unity", std::all_of(gain.begin(), gain.end(), [](double value) { return value == 1; }));
}

void startupAndReset() {
    for (double rate : {44100., 48000., 96000., 192000.}) {
        SpectralNode node;
        SpectralNode::Params p; p.maxCutDb = 0; p.depth = 0;
        node.prepare(rate, 97, 1); node.setParams(p);
        check("fresh requested zero cap", node.effectiveCapDb_ == 0 && !node.finalClampBypass_);
        std::array<float, 256> input{}; float* channels[]{input.data()};
        node.process(channels, 1, 256);
        p.maxCutDb = 24; node.setParams(p);
        double expected = 0;
        const double alpha = std::exp(-256/(rate*.020));
        bool timeline = true;
        for (int hop = 0; hop < 400; ++hop) {
            expected = alpha*expected+(1-alpha)*fx::kLegacyBound;
            if (std::abs(expected-fx::kLegacyBound) <= 1e-6) expected = fx::kLegacyBound;
            trackHeap = true; node.process(channels, 1, 256); trackHeap = false;
            timeline &= node.effectiveCapDb_ == expected;
        }
        check("inactive cap timeline and finite Off at sample rate", timeline && node.finalClampBypass_, rate);
        p.maxCutDb = 6; node.setParams(p);
        node.process(channels, 1, 256);
        check("re-enable moves from current Off cap", node.effectiveCapDb_ > 6 && node.effectiveCapDb_ < fx::kLegacyBound);
        for (int hop = 0; hop < 400; ++hop) node.process(channels, 1, 256);
        check("enabled cap finite completion", node.effectiveCapDb_ == 6 && !node.finalClampBypass_);
        trackHeap = true; node.reset(); trackHeap = false;
        check("reset seeds requested enabled cap", node.effectiveCapDb_ == 6 && !node.finalClampBypass_);
        node.prepare(rate, 97, 1);
        check("reprepare seeds requested cap", node.effectiveCapDb_ == 6 && node.hopsDone_ == 0);
    }
    for (int mode : {0, 1}) {
        SpectralNode node;
        node.prepare(48000, 97, 1); node.setDetectionMode(mode);
        SpectralNode::Params p; p.maxCutDb = 0; node.setParams(p);
        std::array<float, 256> input{}; float* channels[]{input.data()};
        for (int hop = 0; hop < 8; ++hop) {
            for (std::size_t n = 0; n < input.size(); ++n) input[n] = static_cast<float>(.3*std::sin(.4*static_cast<double>(n)));
            node.process(channels, 1, 256);
        }
        check("active mono fresh zero cap yields unity gain", std::all_of(node.gain_.begin(), node.gain_.end(), [](double value) { return value == 1; }));
    }
}
}

int main() {
    scalarHistory();
    startupAndReset();
    double audioError = 0, reductionError = 0, boundError = 0;
    bool chunksExact = true;
    for (int mode : {0, 1}) for (bool ms : {false, true}) {
        const auto reference = render(mode, ms, 137, true, audioError, reductionError, boundError);
        for (int chunk : {1, 37, 256, 479, 4096})
            chunksExact &= reference == render(mode, ms, chunk, false, audioError, reductionError, boundError);
    }
    check("Soft/Hard L/R M/S proto audio", audioError < 1e-7, audioError);
    check("both lanes stored reduction proto", reductionError < 1e-5, reductionError);
    check("every active frame/bin bounded", boundError < 1e-10, boundError);
    check("arbitrary/oversized callback exact", chunksExact);
    check("prepared allocation/delete count", allocations == 0 && deletions == 0, static_cast<double>(allocations+deletions));
    return passed ? 0 : 1;
}
