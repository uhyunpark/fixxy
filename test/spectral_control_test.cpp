#include "dsp/SpectralControl.h"
#include "spectral_control_fixtures.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace sc = spectral_control;
namespace f = spectral_control_fixtures;
static bool trackHeap = false;
static std::size_t allocations = 0, deletions = 0;
void* operator new(std::size_t n) {
    if (trackHeap) ++allocations;
    if (auto* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if (trackHeap) ++deletions; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
static int failures = 0;
static double worst = 0.0;
static void check(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}
static void near(double a, double b, const char* label, double tolerance = 1e-12) {
    worst = std::max(worst, std::abs(a-b));
    check(std::isfinite(a) && std::abs(a-b) <= tolerance, label);
}
static sc::Params fixtureParams() {
    sc::Params p;
    p.detail = 5.0; p.detailLo = 2.0; p.detailHi = -1.5;
    p.attack = 4.0; p.release = 6.0;
    p.attackLo = 1.25; p.attackHi = -0.75;
    p.releaseLo = -1.5; p.releaseHi = 0.5;
    return p;
}
static void fixtures() {
    sc::FrequencyWindows windows;
    std::array<double, f::kBins> median{}, smooth{};
    windows.prepare(f::kFrequenciesHz, f::kWidthOct);
    windows.median(f::kAmplitude.data(), median.data());
    for (std::size_t k = 0; k < f::kBins; ++k) {
        const auto c = sc::coefficients(f::kFrequenciesHz[k], fixtureParams(), 48000.0, 64);
        near(c.low, f::kShelfLow[k], "low shelf");
        near(c.high, f::kShelfHigh[k], "high shelf");
        near(c.detail, f::kDetail[k], "detail tilts");
        near(c.widthOct, f::kWidthOct[k], "width");
        near(c.strength, f::kStrength[k], "strength");
        near(c.detectorTau, f::kDetectorTauSeconds[k], "detector time");
        near(c.attackTau, f::kAttackTauSeconds[k], "attack tilts");
        near(c.releaseTau, f::kReleaseTauSeconds[k], "release tilts");
        near(sc::softKnee(sc::exceedance(f::kAmplitude[k], median[k]), 5.0, c.strength),
             f::kSoftReductionDb[k], "soft fixture");
        near(sc::hardReduction(f::kAmplitude[k], 5.0), f::kHardReductionDb[k], "hard fixture");
        const auto linked = sc::link(f::kAmplitude[k], f::kAmplitude[f::kBins-1-k], 0.37);
        near(linked[0], f::kLinked37[k], "link A");
        near(linked[1], f::kLinked37[f::kBins+k], "link B");
        near(sc::rangeMask(f::kFrequenciesHz[k], 300.0, 16000.0), f::kRangeMask[k], "range fixture");
    }
    constexpr std::array<std::array<double, f::kBins>, 3> masks{{
        {0, .2, .8, 1, .5, 0, 0, 0, 0}, {0, 0, 0, .1, .7, 1, .6, 0, 0},
        {0, 0, 0, 0, 0, .2, .8, 1, .4}}};
    constexpr std::array<double, 3> focuses{-.6, 0, .75};
    for (std::size_t k = 0; k < f::kBins; ++k) {
        sc::BandpassMask a, b, centered;
        for (std::size_t i = 0; i < masks.size(); ++i) {
            const auto q = sc::focus(focuses[i]);
            a.add(masks[i][k], q[0]); b.add(masks[i][k], q[1]); centered.add(masks[i][k]);
        }
        near(a.value(), f::kFocusedMaskA[k], "focused mask A");
        near(b.value(), f::kFocusedMaskB[k], "focused mask B");
        near(centered.value(), f::kBandpassUnion[k], "union");
    }
    double cap = 40.0;
    sc::Ballistics state{35.0, 35.0};
    const double pole = sc::framePole(48000.0, 64, .02);
    for (double expected : f::kSmoothedCapDb) {
        cap = sc::smoothCap(cap, 6.0, pole);
        state.clamp(cap);
        near(cap, expected, "cap fixture");
        check(state.output <= cap, "old output state constrained");
    }
    trackHeap = true;
    windows.median(f::kAmplitude.data(), median.data());
    windows.smooth(f::kAmplitude.data(), smooth.data());
    windows.setWidths(f::kWidthOct);
    trackHeap = false;
}
static void identities() {
    near(sc::positiveKnee(-3.0), 0.0, "knee lower endpoint");
    near(sc::positiveKnee(0.0), .75, "knee middle");
    near(sc::positiveKnee(3.0), 3.0, "knee upper endpoint");
    near(sc::hardReduction(1.0, 0.0), 0.0, "Hard depth zero");
    near(sc::softKnee(40.0, 0.0, 1.0), 0.0, "Soft depth zero");
    near(sc::softKnee(40.0, 5.0, 1.0, false), 0.0, "Soft missing/silent detector zero demand");
    near(sc::hardReduction(0.0, 10.0), 0.0, "Hard silence");
    near(sc::hardReduction(1.0, 10.0, 0.0, false), 0.0, "Hard missing detector zero demand");
    near(sc::softKnee(1e3, 10.0, 2.0), 40.0, "Soft design cap");
    near(sc::hardReduction(1.0, 10.0), 40.0, "Hard design cap");
    near(sc::rangeMask(0.0), 1.0, "inactive range DC");
    near(sc::rangeMask(24000.0), 1.0, "inactive range Nyquist");
    near(sc::calibratedAmplitude(2048.0, 2048.0, true), 1.0, "DC and Nyquist calibration");
    near(sc::calibratedAmplitude(1024.0, 2048.0, false), 1.0, "interior calibration");
    near(sc::BandpassMask{}.value(), 1.0, "no bands identity");
    for (double fraction : {0.0, 0.37, 1.0}) {
        const auto identical = sc::link(.3, .3, fraction);
        near(identical[0], .3, "identical link A"); near(identical[1], .3, "identical link B");
    }
    const auto dual = sc::link(.3, 0.0, 0.0), linked = sc::link(.3, 0.0, 1.0);
    near(dual[0], .3, "dual endpoint A"); near(dual[1], 0.0, "dual endpoint B");
    near(linked[0], .3/std::sqrt(2.0), "one-sided linked RMS");
    near(linked[1], linked[0], "shared endpoint");
    sc::BandpassMask off; off.add(1.0, 0.0);
    near(off.value(), 1.0, "focused-away mask identity");
    for (double p : {-1.0, 0.0, 1.0}) {
        auto q = sc::focus(p);
        check(q[0] >= 0 && q[1] >= 0 && std::max(q[0], q[1]) == 1.0, "focus endpoints");
    }
    for (double a : {0.0, 0.1, 1.0}) for (double b : {-1.0, 0.0, .7}) {
        auto ms = sc::midSide(a, b); auto lr = sc::midSide(ms[0], ms[1]);
        near(lr[0], a, "MS round trip L"); near(lr[1], b, "MS round trip R");
        near(ms[0]*ms[0]+ms[1]*ms[1], a*a+b*b, "MS energy");
    }
    for (double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (std::size_t hop : {64u, 128u, 256u}) {
            const auto c = sc::coefficients(1000.0, sc::Params{}, rate, hop);
            near(c.attackTau, .027, "normalized attack 5");
            near(c.releaseTau, .05, "normalized release 5");
            double detector = 0.0;
            sc::Ballistics state;
            for (int frame = 1; frame <= 100; ++frame) {
                detector = sc::detectorStep(12.0, detector, c.detectorPole);
                state.step(12.0, c.attackPole, c.releasePole);
                const double elapsed = static_cast<double>(frame)*static_cast<double>(hop)/rate;
                near(detector, 12.0*(1-std::exp(-elapsed/c.detectorTau)), "detector closed form", 1e-11);
                near(state.output, 12.0*(1-std::exp(-elapsed/c.attackTau)), "attack closed form", 1e-11);
            }
            state = {12.0, 12.0};
            for (int frame = 1; frame <= 100; ++frame) {
                state.step(0.0, c.attackPole, c.releasePole);
                near(state.release, 12.0*std::pow(c.releasePole, frame), "release closed form", 1e-11);
                const double a = c.attackPole, r = c.releasePole;
                const double expected = 12.0*(std::pow(a, frame)+(1-a)*r*(std::pow(a, frame)-std::pow(r, frame))/(a-r));
                near(state.output, expected, "decoupled release closed form", 1e-10);
            }
        }
    }
    for (double frequency : {0.0, 300.0, 1000.0, 16000.0, 24000.0}) {
        sc::Params p;
        const auto baseline = sc::coefficients(frequency, p, 48000.0, 64);
        for (auto member : {&sc::Params::detailLo, &sc::Params::detailHi}) {
            p = {}; p.*member = 1.0;
            const auto c = sc::coefficients(frequency, p, 48000.0, 64);
            check(c.widthOct <= baseline.widthOct && c.strength >= baseline.strength
                  && c.detectorTau >= baseline.detectorTau, "Detail tilt controls all three maps");
            near(c.attackTau, baseline.attackTau, "Detail independent of attack");
            near(c.releaseTau, baseline.releaseTau, "Detail independent of release");
        }
        for (auto member : {&sc::Params::attackLo, &sc::Params::attackHi,
                            &sc::Params::releaseLo, &sc::Params::releaseHi}) {
            p = {}; p.*member = 1.0;
            const auto c = sc::coefficients(frequency, p, 48000.0, 64);
            near(c.widthOct, baseline.widthOct, "timing tilt independent of Detail");
            if (member == &sc::Params::attackLo || member == &sc::Params::attackHi)
                near(c.releaseTau, baseline.releaseTau, "attack tilt independent of release");
            else near(c.attackTau, baseline.attackTau, "release tilt independent of attack");
        }
    }
    // Deep release memory remains intact, as in the verified prototype; final
    // output is constrained after every update, even if that memory is deeper.
    sc::Ballistics deep{35.0, 35.0};
    for (int frame = 0; frame < 100; ++frame) {
        deep.step(0.0, .8, .99); deep.clamp(6.0);
        check(deep.output <= 6.0, "cap after update constrains old envelope");
    }
    deep.reset(); near(deep.output, 0.0, "reset output"); near(deep.release, 0.0, "reset release");
}
static void dense(bool dump, bool benchmark, int scenario = 0) {
    const std::size_t bins = scenario == 1 ? 513 : scenario == 2 ? 4097 : scenario == 3 ? 8193 : 2049;
    const double rate = scenario == 1 ? 44100.0 : scenario == 2 ? 96000.0 : scenario == 3 ? 192000.0 : 48000.0;
    std::vector<double> frequencies(bins), widths(bins), values(bins), median(bins), smooth(bins);
    for (std::size_t k = 0; k < bins; ++k) {
        frequencies[k] = static_cast<double>(k)*rate/static_cast<double>(2*(bins-1));
        widths[k] = sc::coefficients(frequencies[k], fixtureParams(), 48000.0, 64).widthOct;
        values[k] = 0.1 + std::abs(std::sin(static_cast<double>(k)*.137)) + static_cast<double>(k%7)*.03;
        if (scenario == 4) values[k] = static_cast<double>(k%7)*.03;
    }
    sc::FrequencyWindows windows; windows.prepare(frequencies, widths);
    trackHeap = true;
    windows.median(values.data(), median.data()); windows.smooth(values.data(), smooth.data());
    trackHeap = false;
    if (dump) {
        for (std::size_t k = 0; k < bins; ++k)
            std::printf("%.17g %.17g %.17g %.17g %.17g\n", frequencies[k], widths[k], values[k], median[k], smooth[k]);
        return;
    }
    near(median[0], values[0], "median DC pass through");
    near(smooth[0], values[0], "smooth DC pass through");
    // Direct finite-window reference: test all destinations including clipped
    // Nyquist windows, ties, and odd/even counts. The same dense case is exported
    // by --dump for direct comparison to the verified Python implementation.
    std::vector<double> scratch;
    for (std::size_t k = 0; k < bins; ++k) {
        const double frequency = frequencies[k];
        const auto begin = frequencies.begin(), end = frequencies.end();
        auto lo = frequency == 0 ? begin : std::lower_bound(begin, end, frequency/std::sqrt(2.0));
        auto hi = frequency == 0 ? begin+1 : std::upper_bound(begin, end, frequency*std::sqrt(2.0));
        scratch.assign(values.begin()+(lo-begin), values.begin()+(hi-begin));
        std::sort(scratch.begin(), scratch.end());
        const auto n = scratch.size();
        const double expected = n%2 != 0 ? scratch[n/2] : (scratch[n/2-1]+scratch[n/2])*.5;
        near(median[k], expected, "dense exact median reference");
        lo = frequency == 0 ? begin : std::lower_bound(begin, end, frequency*std::pow(2.0, -widths[k]*.5));
        hi = frequency == 0 ? begin+1 : std::upper_bound(begin, end, frequency*std::pow(2.0, widths[k]*.5));
        double sum = 0.0;
        for (auto index = lo-begin; index < hi-begin; ++index) sum += values[static_cast<std::size_t>(index)]/static_cast<double>(hi-lo);
        near(smooth[k], sum, "dense direct weighted smoothing");
    }
    auto inplace = values;
    windows.smooth(inplace.data(), inplace.data());
    for (std::size_t k = 0; k < bins; ++k) near(inplace[k], smooth[k], "in-place smoothing");
    std::fill(values.begin(), values.end(), 0.0);
    windows.median(values.data(), median.data()); windows.smooth(values.data(), smooth.data());
    check(std::all_of(median.begin(), median.end(), [](double v){ return v == 0; }), "median silence");
    check(std::all_of(smooth.begin(), smooth.end(), [](double v){ return v == 0; }), "smooth silence");
    if (benchmark) {
        std::vector<sc::Ballistics> states(4*bins);
        std::vector<sc::Coefficients> coefficients(bins);
        std::vector<double> detector(4*bins);
        for (std::size_t k = 0; k < bins; ++k) values[k] = .02 + .01*std::sin(static_cast<double>(k)*.037);
        for (std::size_t hop : {256u, 128u, 64u}) {
            for (std::size_t k = 0; k < bins; ++k) coefficients[k] = sc::coefficients(frequencies[k], fixtureParams(), 48000.0, hop);
            for (int channels : {1, 2, 4}) {
                constexpr int frames = 100;
                double total = 0.0, maximum = 0.0;
                for (int frame = -20; frame < frames; ++frame) {
                    const auto start = std::chrono::steady_clock::now();
                    for (int channel = 0; channel < channels; ++channel) {
                        const auto offset = static_cast<std::size_t>(channel)*bins;
                        windows.median(values.data(), median.data());
                        for (std::size_t k = 0; k < bins; ++k) {
                            detector[offset+k] = sc::detectorStep(sc::exceedance(values[k], median[k]), detector[offset+k], coefficients[k].detectorPole);
                            median[k] = sc::softKnee(detector[offset+k], 5.0, coefficients[k].strength);
                        }
                        windows.smooth(median.data(), smooth.data());
                        for (std::size_t k = 0; k < bins; ++k) states[offset+k].step(smooth[k], coefficients[k].attackPole, coefficients[k].releasePole);
                    }
                    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()-start).count();
                    if (frame >= 0) { total += us; maximum = std::max(maximum, us); }
                }
                std::printf("control N4096 H%zu channels=%d mean=%.3f us max=%.3f us budget=%.3f us checksum=%.12g\n",
                            hop, channels, total/frames, maximum, static_cast<double>(hop)/48000.0*1e6, states[999].output);
            }
        }
    }
}
int main(int argc, char** argv) {
    const bool dump = argc > 1 && std::strcmp(argv[1], "--dump") == 0;
    const bool benchmark = argc > 1 && std::strcmp(argv[1], "--benchmark") == 0;
    if (dump) { dense(true, false, argc > 2 ? std::atoi(argv[2]) : 0); return 0; }
    fixtures(); identities(); dense(false, benchmark);
    check(allocations == 0 && deletions == 0, "prepared operations allocate/delete nothing");
    std::printf("SpectralControlTest %s max_error=%.12g heap=%zu/%zu\n", failures == 0 ? "PASS" : "FAIL", worst, allocations, deletions);
    return failures == 0 ? 0 : 1;
}
