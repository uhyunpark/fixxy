#include "dsp/SpectralTransition.h"
#include "dsp/SpectralControl.h"
#include "spectral_transition_fixtures.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <vector>

namespace f = spectral_transition_fixtures;
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
static double maximumError = 0.0;
static void check(bool condition, const char* label) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}
static void near(double actual, double expected, const char* label, double tolerance = 1e-10) {
    if (tolerance <= 1e-10) maximumError = std::max(maximumError, std::abs(actual-expected));
    check(std::isfinite(actual) && std::abs(actual-expected) < tolerance, label);
}
static void fixtures() {
    SpectralTransition lane;
    lane.prepare(f::kDesignFft, f::kTaps, 64, f::kCurrentReductionDb.data(), 40.0);
    trackHeap = true;
    check(lane.beginHop(f::kTargetReductionDb.data(), 6.0), "first hop accepted");
    check(!lane.beginHop(f::kTargetReductionDb.data(), 6.0), "second admission before fade rejected");
    trackHeap = false;
    near(SpectralTransition::phaseLimit, f::kPhiLimit, "nominal phase limit");
    near(SpectralTransition::admissionLimit, f::kAdmissionPhaseLimit, "fixed admission reserve");
    near(lane.alpha(), f::kFixtureAlpha, "fixture alpha");
    near(lane.gridDistance(), f::kFixtureGridDistance, "unwrapped grid distance");
    near(lane.admittedCap(), f::kNextEffectiveCapDb, "admitted cap");
    for (std::size_t k = 0; k < f::kPositiveBins; ++k)
        near(lane.reduction(k), f::kNextReductionDb[k], "convex log reduction");
    for (std::size_t k = 0; k < f::kTaps; ++k)
        near(lane.admittedTaps()[k], f::kNextTaps[k], "admitted finite taps");
    std::array<double, 64> input{}, output{}; input[0] = 1.0;
    near(lane.effectiveCap(), 40.0, "fade begins at current cap");
    lane.process(input.data(), output.data(), 32);
    near(lane.effectiveCap(), .5*(40.0+f::kNextEffectiveCapDb), "half fade cap");
    lane.process(input.data()+32, output.data()+32, 32);
    near(lane.effectiveCap(), f::kNextEffectiveCapDb, "boundary cap exact");
    check(lane.remaining() == 0 && lane.admissions() == 1, "one new IR per hop");
}
static double dotAt(std::span<const double> input, std::span<const double> taps, std::size_t sample) {
    double sum = 0.0;
    for (std::size_t k = 0; k < taps.size() && k <= sample; ++k) sum += taps[k]*input[sample-k];
    return sum;
}
static void history() {
    SpectralFirKernel oldKernel, newKernel;
    oldKernel.prepare(f::kHistoryOldTaps.data(), f::kHistoryTaps);
    newKernel.prepare(f::kHistoryNewTaps.data(), f::kHistoryTaps);
    SpectralFirConvolver oldBank, newBank, warmed;
    oldBank.prepare(oldKernel); newBank.prepare(oldKernel); warmed.prepare(newKernel);
    std::vector<double> a(f::kHistorySamples), b(a.size()), full(a.size());
    constexpr std::array<std::size_t, 8> chunks{17, 91, 3, 205, 64, 11, 129, 7};
    std::size_t at = 0, chunk = 0;
    double fixtureError = 0.0, activationError = 0.0;
    trackHeap = true;
    while (at < a.size()) {
        if (at == f::kTransitionStart) newBank.replaceAtBoundary(newKernel);
        auto count = std::min(chunks[chunk++%chunks.size()], a.size()-at);
        if (at < f::kTransitionStart) count = std::min(count, f::kTransitionStart-at);
        oldBank.process(f::kHistoryInput.data()+at, a.data()+at, count);
        newBank.process(f::kHistoryInput.data()+at, b.data()+at, count);
        warmed.process(f::kHistoryInput.data()+at, full.data()+at, count);
        for (std::size_t n = at; n < at+count; ++n) {
            const double q = static_cast<double>(n)-static_cast<double>(f::kTransitionStart);
            const double t = q < 0 ? 0 : q >= 64 ? 1 : .5-.5*std::cos(std::numbers::pi*q/64.0);
            fixtureError = std::max(fixtureError, std::abs((1-t)*a[n]+t*b[n]-f::kHistoryExpected[n]));
            if (n >= f::kTransitionStart) activationError = std::max(activationError, std::abs(b[n]-full[n]));
        }
        at += count;
    }
    trackHeap = false;
    near(fixtureError, 0.0, "generated history fixture");
    near(activationError, 0.0, "new bank equals kernel warmed from sample zero");
    std::printf("history fixture=%.12g warm activation=%.12g\n", fixtureError, activationError);
}
static void makeTarget(std::span<double> target, double sampleRate, std::size_t frame) {
    const double grid = static_cast<double>(2*(target.size()-1));
    for (std::size_t k = 0; k < target.size(); ++k) {
        const double hz = static_cast<double>(k)*sampleRate/grid;
        if (frame%4 == 0) target[k] = 40.0/(1.0+std::pow(hz/1000.0, 4.0));
        else if (frame%4 == 1) target[k] = 40.0/(1.0+std::pow(10000.0/std::max(hz, 1e-9), 4.0));
        else if (frame%4 == 2) target[k] = 40.0*std::exp(-.5*std::pow((hz-3000.0-.5*sampleRate/grid)/(54.0/2.355), 2.0));
        else target[k] = 0.0;
    }
}
static void dynamic() {
    constexpr std::size_t size = 16384, taps = 4096, activeSamples = 8192;
    std::vector<double> target(size/2+1, 0.0), old(taps), next(taps);
    std::vector<double> input(activeSamples+taps), output(input.size());
    for (std::size_t n = 0; n < activeSamples; ++n)
        input[n] = .1*std::sin(static_cast<double>(n)*.137)+.05*std::cos(static_cast<double>(n)*.73);
    input[0] = 1.0;
    constexpr std::array<std::size_t, 9> chunks{1, 17, 91, 3, 511, 64, 205, 7, 129};
    for (std::size_t hop : {64u, 128u, 256u}) {
        std::fill(target.begin(), target.end(), 0.0);
        SpectralTransition lane; lane.prepare(size, taps, hop, target.data(), 40.0);
        std::copy(lane.admittedTaps().begin(), lane.admittedTaps().end(), next.begin());
        std::size_t at = 0, chunk = 0, frame = 0, q = 0;
        double error = 0.0;
        while (at < input.size()) {
            const auto stop = std::min(input.size(), at+chunks[chunk++%chunks.size()]);
            while (at < stop) {
                if (lane.remaining() == 0) {
                    std::copy(next.begin(), next.end(), old.begin());
                    if (at < activeSamples) makeTarget(target, 48000.0, frame);
                    else std::fill(target.begin(), target.end(), 0.0);
                    trackHeap = true;
                    check(lane.beginHop(target.data(), frame%3 == 0 ? 6.0 : frame%3 == 1 ? 12.0 : 40.0), "latest target admitted each hop");
                    trackHeap = false;
                    std::copy(lane.admittedTaps().begin(), lane.admittedTaps().end(), next.begin());
                    q = 0; ++frame;
                }
                const auto count = std::min(stop-at, lane.remaining());
                trackHeap = true;
                lane.process(input.data()+at, output.data()+at, count);
                trackHeap = false;
                for (std::size_t n = 0; n < count; ++n) {
                    const double weight = .5-.5*std::cos(std::numbers::pi*static_cast<double>(q+n)/static_cast<double>(hop));
                    const double reference = (1-weight)*dotAt(input, old, at+n)+weight*dotAt(input, next, at+n);
                    error = std::max(error, std::abs(output[at+n]-reference));
                }
                at += count; q += count;
            }
        }
        near(error, 0.0, "dynamic direct sum with latest targets and history wrap");
        near(output[0], input[0], "zero additional buffer delay");
        check(lane.admissions() == input.size()/hop, "exactly one admission per actual H");
        std::printf("dynamic H%zu direct=%.12g admissions=%zu\n", hop, error, lane.admissions());
    }
}
static void fallingCaps() {
    for (std::size_t hop : {64u, 128u, 256u}) {
        std::vector<double> target(513, 40.0);
        std::array<double, 256> input{}, output{}; input.fill(1.0);
        SpectralTransition lane; lane.prepare(1024, 256, hop, target.data(), 40.0);
        double maximumOvershoot = 0.0, requestGap = 0.0;
        for (std::size_t frame = 1; frame <= 160; ++frame) {
            const double requested = 6.0+34.0*std::exp(-static_cast<double>(frame*hop)/960.0);
            lane.beginHop(target.data(), requested);
            near(lane.alpha(), 1.0, "phase-free scalar admission");
            for (std::size_t q = 0; q < hop; ++q) {
                const double effective = lane.effectiveCap();
                lane.process(input.data(), output.data(), 1);
                maximumOvershoot = std::max(maximumOvershoot, -20.0*std::log10(output[0])-effective);
                requestGap = std::max(requestGap, effective-requested);
            }
        }
        check(maximumOvershoot <= .05 && requestGap > 0.0, "effective cap guarantee differs from requested");
        near(lane.effectiveCap(), 6.0, "falling cap settles", .001);
        std::printf("falling H%zu effective overshoot=%.12g request gap=%.12g\n", hop, maximumOvershoot, requestGap);
    }
}
static void impulseTransitions() {
    constexpr std::size_t start = 12*64, hop = 64, samples = start+hop+512;
    SpectralFirKernel oldKernel, newKernel;
    oldKernel.prepare(f::kHistoryOldTaps.data(), f::kHistoryTaps);
    newKernel.prepare(f::kHistoryNewTaps.data(), f::kHistoryTaps);
    double error = 0.0;
    for (std::size_t offset = 0; offset < 64; ++offset) {
        SpectralFirConvolver a, b; a.prepare(oldKernel); b.prepare(oldKernel);
        std::array<double, samples> x{}, old{}, next{};
        // One impulse in retained pre-transition history; another in the fade.
        x[start-128+offset] = .7; x[start+offset] = -.3;
        a.process(x.data(), old.data(), start); b.process(x.data(), next.data(), start);
        b.replaceAtBoundary(newKernel);
        a.process(x.data()+start, old.data()+start, samples-start);
        b.process(x.data()+start, next.data()+start, samples-start);
        for (std::size_t n = start; n < samples; ++n) {
            error = std::max(error, std::abs(next[n]-dotAt(x, f::kHistoryNewTaps, n)));
            error = std::max(error, std::abs(old[n]-dotAt(x, f::kHistoryOldTaps, n)));
        }
    }
    near(error, 0.0, "every partition input offset before and during activation");
    std::printf("activation all 64 impulse offsets=%.12g\n", error);
}
static void dumpSequence(std::size_t scale) {
    const std::size_t size = 16384*scale, taps = 4096*scale;
    std::vector<double> target(size/2+1, 0.0);
    SpectralTransition lane; lane.prepare(size, taps, 64, target.data(), 40.0);
    std::array<double, 64> x{}, y{};
    for (std::size_t frame = 0; frame < 12; ++frame) {
        makeTarget(target, 48000.0*static_cast<double>(scale), frame);
        lane.beginHop(target.data(), frame%3 == 0 ? 6.0 : frame%3 == 1 ? 12.0 : 40.0);
        std::printf("%.17g %.17g %.17g", lane.alpha(), lane.admittedCap(), lane.gridDistance());
        for (double tap : lane.admittedTaps()) std::printf(" %.17g", tap);
        std::printf("\n");
        lane.process(x.data(), y.data(), 64);
    }
}
struct BenchmarkControl {
    std::size_t size = 0, hopSize = 0;
    double windowSum = 0.0, sampleRate = 0.0, parameterPole = 0.0;
    spectral_control::Params parameters;
    SpectralFft fft;
    spectral_control::FrequencyWindows windows;
    std::vector<double> source, window, frame, real, imag, amplitude, background, detector, smooth;
    std::vector<spectral_control::Coefficients> coeff;
    std::vector<spectral_control::Ballistics> state;
    std::vector<double> frequencies, widths;
    void prepare(double rate, std::size_t n, std::size_t hop) {
        size = n; fft.prepare(static_cast<int>(n));
        sampleRate = rate; hopSize = hop;
        parameterPole = spectral_control::framePole(rate, hop, .020);
        source.resize(n); window.resize(n); frame.resize(n);
        const auto bins = n/2+1;
        real.resize(bins); imag.resize(bins); amplitude.resize(bins); background.resize(bins);
        detector.assign(bins, 0.0); smooth.resize(bins); coeff.resize(bins); state.resize(bins);
        frequencies.resize(bins); widths.resize(bins);
        for (std::size_t k = 0; k < bins; ++k) {
            frequencies[k] = static_cast<double>(k)*rate/static_cast<double>(n);
            coeff[k] = spectral_control::coefficients(frequencies[k], {}, rate, hop);
            widths[k] = coeff[k].widthOct;
        }
        windows.prepare(frequencies, widths);
        for (std::size_t i = 0; i < n; ++i) {
            window[i] = .5-.5*std::cos(2.0*std::numbers::pi*static_cast<double>(i)/static_cast<double>(n));
            windowSum += window[i];
            source[i] = .1*std::sin(static_cast<double>(i)*.137)+.03*std::cos(static_cast<double>(i)*.397);
        }
    }
    void configure(std::size_t iteration, bool automate) {
        if (automate) {
            const double u = std::sin(static_cast<double>(iteration)*.11);
            const auto smoothParameter = [this](double old, double wanted) {
                return parameterPole*old+(1.0-parameterPole)*wanted;
            };
            parameters.detail = smoothParameter(parameters.detail, 5.0+4.0*u);
            parameters.attack = smoothParameter(parameters.attack, 5.0+3.0*u);
            parameters.release = smoothParameter(parameters.release, 5.0-3.0*u);
            parameters.detailLo = smoothParameter(parameters.detailLo, 4.0*u);
            parameters.detailHi = smoothParameter(parameters.detailHi, -4.0*u);
            parameters.attackLo = smoothParameter(parameters.attackLo, 1.5*u);
            parameters.attackHi = smoothParameter(parameters.attackHi, -1.5*u);
            parameters.releaseLo = smoothParameter(parameters.releaseLo, -1.5*u);
            parameters.releaseHi = smoothParameter(parameters.releaseHi, 1.5*u);
            for (std::size_t k = 0; k < real.size(); ++k) {
                coeff[k] = spectral_control::coefficients(frequencies[k], parameters, sampleRate, hopSize);
                widths[k] = coeff[k].widthOct;
            }
            windows.setWidths(widths);
        }
    }
    void analyze(std::span<double> target, double cap, std::size_t iteration) {
        for (std::size_t i = 0; i < size; ++i) frame[i] = source[(i+iteration)%size]*window[i];
        fft.forward(frame.data(), real.data(), imag.data());
        for (std::size_t k = 0; k < real.size(); ++k)
            amplitude[k] = spectral_control::calibratedAmplitude(std::hypot(real[k], imag[k]), windowSum, k == 0 || k+1 == real.size());
        windows.median(amplitude.data(), background.data());
        for (std::size_t k = 0; k < real.size(); ++k) {
            detector[k] = spectral_control::detectorStep(spectral_control::exceedance(amplitude[k], background[k]), detector[k], coeff[k].detectorPole);
            background[k] = spectral_control::softKnee(detector[k], 5.0, coeff[k].strength);
        }
        windows.smooth(background.data(), smooth.data());
        for (std::size_t k = 0; k < real.size(); ++k) {
            state[k].step(smooth[k], coeff[k].attackPole, coeff[k].releasePole);
            state[k].clamp(cap); smooth[k] = state[k].output;
        }
        const double ratio = static_cast<double>(real.size()-1)/static_cast<double>(target.size()-1);
        for (std::size_t k = 0; k < target.size(); ++k) {
            const double position = static_cast<double>(k)*ratio;
            const auto lo = static_cast<std::size_t>(position);
            const auto hi = std::min(lo+1, smooth.size()-1);
            target[k] = smooth[lo]+(position-static_cast<double>(lo))*(smooth[hi]-smooth[lo]);
        }
    }
    void update(std::span<double> target, double cap, std::size_t iteration, bool automate) {
        configure(iteration, automate);
        analyze(target, cap, iteration);
    }
};
static void benchmark(bool only48, bool automate, bool highOnly, bool scaled = false) {
    constexpr std::array<double, 2> baseRates{48000.0, 44100.0};
    for (double baseRate : baseRates) {
        if (!scaled && baseRate != 48000.0) continue;
    for (std::size_t scale : {1u, 2u, 4u}) {
        if (only48 && scale != 1) continue;
        if (highOnly && scale == 1) continue;
        const double rate = baseRate*static_cast<double>(scale);
        for (std::size_t baseHop : {256u, 128u, 64u}) {
            const std::size_t hop = baseHop*(scaled ? scale : 1);
            const std::size_t size = 16384*scale, taps = 4096*scale;
            if (hop > 256) {
                std::printf("SCALED backend=%s automation=%s fs=%.0f M=%zu L=%zu H=%zu UNSUPPORTED transition_capacity=256\n",
                            SPECTRALFFT_USE_VDSP ? "vDSP" : "scalar", automate ? "on" : "off", rate, size, taps, hop);
                continue;
            }
            std::array<SpectralTransition, 2> lanes;
            std::array<BenchmarkControl, 2> controls;
            std::array<std::vector<double>, 2> targets;
            for (std::size_t ch = 0; ch < 2; ++ch) {
                targets[ch].assign(size/2+1, 0.0);
                lanes[ch].prepare(size, taps, hop, targets[ch].data(), 40.0);
                controls[ch].prepare(rate, 4096*scale, hop);
            }
            std::array<double, 256> input{}, output{};
            for (std::size_t i = 0; i < hop; ++i) input[i] = .1*std::sin(static_cast<double>(i)*.13);
            constexpr std::size_t repetitions = 100;
            std::array<double, repetitions> elapsed{}, controlTime{}, updateTime{}, renderTime{}, coefficientTime{}, analysisTime{};
            double cap = 40.0;
            const double capPole = spectral_control::framePole(rate, hop, .02);
            // The scaled probe fills the entire J+1 history before sampling.
            const std::size_t warmup = scaled ? std::max<std::size_t>(20, taps/hop+2) : 20;
            for (std::size_t iteration = 0; iteration < repetitions+warmup; ++iteration) {
                const auto start = std::chrono::steady_clock::now();
                cap = spectral_control::smoothCap(cap, iteration%20 < 10 ? 6.0 : 40.0, capPole);
                auto afterCoefficients = start;
                if (scaled) {
                    for (std::size_t ch = 0; ch < 2; ++ch) controls[ch].configure(iteration+ch*137, automate);
                    afterCoefficients = std::chrono::steady_clock::now();
                    for (std::size_t ch = 0; ch < 2; ++ch) controls[ch].analyze(targets[ch], cap, iteration+ch*137);
                } else {
                    for (std::size_t ch = 0; ch < 2; ++ch) controls[ch].update(targets[ch], cap, iteration+ch*137, automate);
                }
                const auto afterControl = std::chrono::steady_clock::now();
                for (std::size_t ch = 0; ch < 2; ++ch) lanes[ch].beginHop(targets[ch].data(), cap);
                const auto afterUpdate = std::chrono::steady_clock::now();
                for (auto& lane : lanes) lane.process(input.data(), output.data(), hop);
                const auto end = std::chrono::steady_clock::now();
                if (iteration >= warmup) {
                    const auto i = iteration-warmup;
                    elapsed[i] = std::chrono::duration<double, std::micro>(end-start).count();
                    controlTime[i] = std::chrono::duration<double, std::micro>(afterControl-start).count();
                    updateTime[i] = std::chrono::duration<double, std::micro>(afterUpdate-afterControl).count();
                    renderTime[i] = std::chrono::duration<double, std::micro>(end-afterUpdate).count();
                    coefficientTime[i] = std::chrono::duration<double, std::micro>(afterCoefficients-start).count();
                    analysisTime[i] = std::chrono::duration<double, std::micro>(afterControl-afterCoefficients).count();
                }
            }
            auto mean = [](const auto& values) { double sum = 0; for (double v : values) sum += v; return sum/static_cast<double>(values.size()); };
            const double average = mean(elapsed);
            std::sort(elapsed.begin(), elapsed.end());
            std::printf("FULL backend=%s automation=%s fs=%.0f M=%zu L=%zu H=%zu mean=%.3f median=%.3f p99=%.3f max=%.3f us deadline=%.3f controls=%.3f update=%.3f render4=%.3f checksum=%.12g\n",
                        SPECTRALFFT_USE_VDSP ? "vDSP" : "scalar", automate ? "on" : "off", rate, size, taps, hop,
                        average, .5*(elapsed[49]+elapsed[50]), elapsed[98], elapsed[99],
                        static_cast<double>(hop)/rate*1e6, mean(controlTime), mean(updateTime), mean(renderTime), output[hop-1]);
            std::fflush(stdout);
            if (scaled) {
                const auto printPhase = [&mean](const char* label, auto values) {
                    const double averagePhase = mean(values);
                    std::sort(values.begin(), values.end());
                    std::printf("PHASE name=%s mean=%.3f p99=%.3f max=%.3f us\n", label, averagePhase, values[98], values[99]);
                };
                std::printf("SCALED baseH=%zu warmup=%zu measured=%zu\n", baseHop, warmup, repetitions);
                printPhase("cap_and_coefficient_setup", coefficientTime);
                printPhase("analysis_detection_interpolation", analysisTime);
                printPhase("admission_synthesis_refresh_history", updateTime);
                printPhase("four_bank_render", renderTime);
                std::fflush(stdout);
            }
        }
    }
    }
}
int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--benchmark-scaled") == 0) {
        benchmark(false, false, false, true);
        benchmark(false, true, false, true);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--dump-sequence") == 0) {
        dumpSequence(argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2])) : 1); return 0;
    }
    if (argc > 1 && (std::strcmp(argv[1], "--benchmark") == 0 || std::strcmp(argv[1], "--benchmark48") == 0
                    || std::strcmp(argv[1], "--benchmark-high") == 0 || std::strcmp(argv[1], "--benchmark-automation") == 0)) {
        benchmark(std::strcmp(argv[1], "--benchmark48") == 0,
                  std::strcmp(argv[1], "--benchmark-automation") == 0,
                  std::strcmp(argv[1], "--benchmark-high") == 0); return 0;
    }
    fixtures();
    history(); dynamic(); fallingCaps(); impulseTransitions();
    check(allocations == 0 && deletions == 0, "no allocation or delete in update/render");
    std::printf("SpectralTransitionTest %s error=%.12g heap=%zu/%zu\n",
                failures == 0 ? "PASS" : "FAIL", maximumError, allocations, deletions);
    return failures == 0 ? 0 : 1;
}
