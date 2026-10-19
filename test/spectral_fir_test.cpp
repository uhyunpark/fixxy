// Fixtures catch cepstral sign/scaling errors; literal convolution and offset
// impulses catch delayed heads, stale histories, and overlap/ring indexing errors.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <vector>

#include "dsp/SpectralFir.h"
#include "spectral_fir_fixtures.h"

namespace f = spectral_fir_fixtures;
static bool trackHeap = false;
static std::size_t allocations = 0, deallocations = 0;
void* operator new(std::size_t n) {
    if (trackHeap) ++allocations;
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {
    if (trackHeap) ++deallocations;
    std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

static int failures = 0;
static void check(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}
static double maxError(std::span<const double> actual, std::span<const double> expected) {
    double error = 0.0;
    check(actual.size() == expected.size(), "matching lengths");
    for (std::size_t i = 0; i < std::min(actual.size(), expected.size()); ++i) {
        if (!std::isfinite(actual[i])) return HUGE_VAL;
        error = std::max(error, std::abs(actual[i] - expected[i]));
    }
    return error;
}
static std::vector<double> direct(std::span<const double> x, std::span<const double> h) {
    std::vector<double> y(x.size(), 0.0);
    for (std::size_t n = 0; n < x.size(); ++n)
        for (std::size_t k = 0; k < h.size() && k <= n; ++k) y[n] += h[k] * x[n-k];
    return y;
}
static void chunks(SpectralFirConvolver& fir, std::span<const double> x, std::span<double> y) {
    constexpr std::array<std::size_t, 9> sizes{1, 17, 64, 3, 511, 128, 7, 65, 256};
    std::size_t pos = 0, block = 0;
    while (pos < x.size()) {
        const auto count = std::min(sizes[block++ % sizes.size()], x.size()-pos);
        fir.process(x.data()+pos, y.data()+pos, count);
        pos += count;
    }
}
static void synthesisTest() {
    SpectralFirSynthesis synthesis;
    synthesis.prepare(f::kDesignFft, f::kMinimumTaps);
    std::vector<double> taps(f::kMinimumTaps), zero(f::kTargetReductionDb.size(), 0.0);
    synthesis.minimumPhase(f::kTargetReductionDb.data(), taps.data());
    const double error = maxError(taps, f::kMinimumPhaseTaps);
    check(error < 1e-10, "generated minimum-phase taps");
    SpectralFirKernel kernel;
    kernel.prepare(taps.data(), taps.size());
    double spectrumError = 0.0;
    spectrumError = std::max(spectrumError, maxError(kernel.real(0), f::kPartition0Real));
    spectrumError = std::max(spectrumError, maxError(kernel.imag(0), f::kPartition0Imag));
    spectrumError = std::max(spectrumError, maxError(kernel.real(1), f::kPartition1Real));
    spectrumError = std::max(spectrumError, maxError(kernel.imag(1), f::kPartition1Imag));
    check(spectrumError < 1e-10, "generated partition spectra");
    synthesis.minimumPhase(zero.data(), taps.data());
    check(taps[0] == 1.0 && std::all_of(taps.begin()+1, taps.end(), [](double v) { return v == 0.0; }),
          "exact unity synthesis");
    std::fill(zero.begin(), zero.end(), 100.0);
    synthesis.minimumPhase(zero.data(), taps.data());
    check(std::abs(taps[0]-0.01) < 1e-13, "40 dB design cap");
    check(std::all_of(taps.begin()+1, taps.end(), [](double v) { return std::abs(v) < 1e-13; }),
          "constant attenuation has no delayed taps");
    std::fill(zero.begin(), zero.end(), -12.0);
    synthesis.minimumPhase(zero.data(), taps.data());
    check(taps[0] == 1.0, "negative reduction clamps to unity");
    std::printf("synthesis taps max=%.12g partition spectra max=%.12g\n", error, spectrumError);
}
static void rendererTest() {
    SpectralFirKernel kernel;
    kernel.prepare(f::kRendererTaps.data(), f::kRendererTaps.size());
    SpectralFirConvolver fir;
    fir.prepare(kernel);
    std::vector<double> out(f::kRendererInput.size());
    chunks(fir, f::kRendererInput, out);
    double error = maxError(out, f::kRendererExpected);
    check(error < 1e-10, "SciPy generated renderer vector");
    std::vector<double> taps(f::kMinimumTaps);
    for (std::size_t i = 0; i < taps.size(); ++i)
        taps[i] = 0.007 * std::sin(static_cast<double>(i)*0.19);
    for (const auto k : {63u, 64u, 127u, 128u, 4095u}) taps[k] = 0.21;
    kernel.prepare(taps.data(), taps.size());
    fir.prepare(kernel);
    std::vector<double> x(3*taps.size()+129, 0.0), y(x.size());
    for (std::size_t i = 0; i < 2*taps.size(); ++i)
        x[i] = 0.2 * std::sin(static_cast<double>(i)*0.413) + 0.1 * std::cos(static_cast<double>(i)*0.039);
    auto expected = direct(x, taps);
    trackHeap = true;
    chunks(fir, x, y);
    trackHeap = false;
    error = std::max(error, maxError(y, expected));
    check(error < 1e-10, "4096-tap full-tail independent direct sum and ring wrap");
    fir.reset();
    auto inplace = x;
    fir.process(inplace.data(), inplace.data(), inplace.size());
    check(maxError(inplace, expected) < 1e-10, "reset and in-place host chunk equivalence");
    double impulseError = 0.0;
    // All P offsets in a non-initial block, plus complete FIR decay and silence.
    for (std::size_t offset = 0; offset < f::kPartition; ++offset) {
        fir.reset();
        std::fill(x.begin(), x.end(), 0.0);
        std::fill(expected.begin(), expected.end(), 0.0);
        x[2*f::kPartition+offset] = 1.0;
        std::copy(taps.begin(), taps.end(), expected.begin()+static_cast<std::ptrdiff_t>(2*f::kPartition+offset));
        chunks(fir, x, y);
        impulseError = std::max(impulseError, maxError(y, expected));
    }
    check(impulseError < 1e-10, "all 64 impulse offsets with full 4096-tap tail");
    // Nonmultiple tap count exercises final partition zero-padding; short head too.
    for (const std::size_t length : {std::size_t{1}, std::size_t{17}, std::size_t{65}, std::size_t{4097}}) {
        std::vector<double> odd(length, 0.0);
        odd[0] = 1.0;
        if (length > 1) odd.back() = -0.2;
        kernel.prepare(odd.data(), odd.size());
        fir.prepare(kernel);
        std::fill(x.begin(), x.end(), 0.0);
        x[0] = 0.7; x[127] = -0.4;
        expected = direct(x, odd);
        chunks(fir, x, y);
        check(maxError(y, expected) < 1e-10, "partial partition and short FIR");
        check(y[0] == x[0], "unity/direct head zero buffer latency");
    }
    trackHeap = true;
    fir.reset();
    fir.process(nullptr, nullptr, 0);
    chunks(fir, x, y);
    trackHeap = false;
    check(allocations == 0 && deallocations == 0, "render/reset no C++ allocation or delete");
    std::printf("renderer max=%.12g impulse offsets max=%.12g heap=%zu/%zu\n",
                error, impulseError, allocations, deallocations);
}
static void benchmark() {
    SpectralFirSynthesis synthesis;
    synthesis.prepare(f::kDesignFft, f::kMinimumTaps);
    SpectralFirKernel kernel;
    kernel.prepare(f::kMinimumPhaseTaps.data(), f::kMinimumPhaseTaps.size());
    std::array<SpectralFirConvolver, 4> banks;
    for (auto& bank : banks) bank.prepare(kernel);
    std::vector<double> taps(f::kMinimumTaps), input(256), output(256);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = 0.2*std::sin(static_cast<double>(i)*0.13);
    using Clock = std::chrono::steady_clock;
    double checksum = 0.0;
    for (const std::size_t hop : {256u, 128u, 64u}) {
        std::vector<double> renderUs, synthesisUs, combinedUs;
        constexpr int iterations = 512;
        renderUs.reserve(iterations); synthesisUs.reserve(iterations); combinedUs.reserve(iterations);
        for (int i = 0; i < iterations+16; ++i) {
            const auto t0 = Clock::now();
            for (auto& bank : banks) bank.process(input.data(), output.data(), hop);
            const auto t1 = Clock::now();
            // Two channel-local IR syntheses per control hop, conservatively.
            synthesis.minimumPhase(f::kTargetReductionDb.data(), taps.data());
            checksum += taps[0] + output[hop-1];
            synthesis.minimumPhase(f::kTargetReductionDb.data(), taps.data());
            const auto t2 = Clock::now();
            if (i >= 16) {
                renderUs.push_back(std::chrono::duration<double, std::micro>(t1-t0).count());
                synthesisUs.push_back(std::chrono::duration<double, std::micro>(t2-t1).count());
                combinedUs.push_back(std::chrono::duration<double, std::micro>(t2-t0).count());
            }
        }
        const auto stats = [](std::vector<double>& v) {
            double sum = 0; for (const double value : v) sum += value;
            std::sort(v.begin(), v.end());
            return std::array<double, 4>{sum/static_cast<double>(v.size()), v[v.size()/2], v[99*v.size()/100], v.back()};
        };
        const auto r = stats(renderUs), s = stats(synthesisUs), c = stats(combinedUs);
        std::printf("BENCH H=%zu deadline_us=%.3f render4_mean/p50/p99/max=%.3f/%.3f/%.3f/%.3f synthesis2=%.3f/%.3f/%.3f/%.3f combined=%.3f/%.3f/%.3f/%.3f\n",
                    hop, 1e6*static_cast<double>(hop)/f::kSampleRate,
                    r[0],r[1],r[2],r[3],s[0],s[1],s[2],s[3],c[0],c[1],c[2],c[3]);
    }
    std::printf("benchmark checksum=%.9f\n", checksum);
}
int main(int argc, char**) {
    std::printf("SpectralFirTest backend=%s M=%zu L=%zu\n",
                SPECTRALFFT_USE_VDSP ? "vDSP" : "scalar", f::kDesignFft, f::kMinimumTaps);
    synthesisTest(); rendererTest();
    if (argc > 1) benchmark();
    std::printf("%s SpectralFirTest (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
