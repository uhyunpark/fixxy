// Offline functional check of AnalyzerEngine (the Stage-2 spectrum engine).
// Deterministic harness: push known signals straight into the engine and assert
// on its dB output. Needs juce::dsp::FFT, so it is a JUCE console app linking
// juce_dsp only (CMakeLists.txt) — kept out of the plugin target.
//
//   1) Two-tone: on-bin sines must peak at the correct bins, at the level the
//      2/N Hann scaling implies (uncompensated coherent gain -> -6 dB), ±1 dB.
//   2) Tilt: flat-power white noise must read a +kTiltDbPerOct dB/oct slope.
//   3) Pink: -3 dB/oct pink noise reads (tilt - 3) dB/oct -> NOT flat at 4.5;
//      flat only if the tilt were 3 dB/oct. Documents the design discrepancy.

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "plugins/eq/ui/AnalyzerEngine.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;
constexpr int kN = AnalyzerEngine::kFftSize;
const double kBinHz = kFs / kN;

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

// Least-squares slope of y vs x over [k0, k1].
double slope(const std::vector<float>& y, int k0, int k1) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (int k = k0; k <= k1; ++k) {
        const double x = std::log2(k * kBinHz);  // octaves
        const double v = y[static_cast<std::size_t>(k)];
        sx += x; sy += v; sxx += x * x; sxy += x * v;
        ++n;
    }
    return (n * sxy - sx * sy) / (n * sxx - sx * sx);
}

void twoTone() {
    std::printf("Two-tone (on-bin sines, 1 channel):\n");
    AnalyzerEngine eng;
    eng.setSampleRate(kFs);

    const int kA = 85, kB = 683;           // 996.1 Hz, 8003.9 Hz (exactly on-bin)
    const float amp = 0.5f;
    std::vector<float> buf(static_cast<std::size_t>(kN));
    for (int n = 0; n < kN; ++n)
        buf[static_cast<std::size_t>(n)] =
            amp * (std::sin(2.0 * kPi * kA * n / kN) + std::sin(2.0 * kPi * kB * n / kN));
    const float* ch[1] = {buf.data()};
    eng.pushBlock(ch, 1, kN);
    eng.computeFrame();

    const auto& raw = eng.rawDb();
    auto argmaxNear = [&](int centre) {
        int best = centre;
        for (int k = centre - 3; k <= centre + 3; ++k)
            if (raw[static_cast<std::size_t>(k)] > raw[static_cast<std::size_t>(best)])
                best = k;
        return best;
    };
    const int pA = argmaxNear(kA), pB = argmaxNear(kB);
    // 2/N + Hann coherent gain (0.5, uncompensated): peak = 20*log10(amp/2).
    const double expected = 20.0 * std::log10(amp / 2.0);

    std::printf("  peakA bin=%d (want %d) %.2f dB | peakB bin=%d (want %d) %.2f dB | expected %.2f dB\n",
                pA, kA, raw[static_cast<std::size_t>(pA)], pB, kB, raw[static_cast<std::size_t>(pB)], expected);
    check(pA == kA, "tone A peaks at its bin");
    check(pB == kB, "tone B peaks at its bin");
    check(std::abs(raw[static_cast<std::size_t>(pA)] - expected) < 1.0, "tone A level within 1 dB");
    check(std::abs(raw[static_cast<std::size_t>(pB)] - expected) < 1.0, "tone B level within 1 dB");
}

// Average one-frame binDb over R independent noise realizations (engine reset
// each run so no decay carries between frames).
std::vector<float> averageSpectrum(bool pink, int runs) {
    std::mt19937 rng(1234567u);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<double> acc(static_cast<std::size_t>(AnalyzerEngine::kNumBins), 0.0);
    std::vector<float> buf(static_cast<std::size_t>(kN));

    for (int r = 0; r < runs; ++r) {
        // Paul Kellet economical pink filter state (per run).
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (int n = 0; n < kN; ++n) {
            const float w = dist(rng);
            float s = w;
            if (pink) {
                b0 = 0.99886 * b0 + w * 0.0555179;
                b1 = 0.99332 * b1 + w * 0.0750759;
                b2 = 0.96900 * b2 + w * 0.1538520;
                b3 = 0.86650 * b3 + w * 0.3104856;
                b4 = 0.55000 * b4 + w * 0.5329522;
                b5 = -0.7616 * b5 - w * 0.0168980;
                s = static_cast<float>((b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * 0.11);
                b6 = w * 0.115926;
            }
            buf[static_cast<std::size_t>(n)] = s;
        }
        AnalyzerEngine eng;
        eng.setSampleRate(kFs);
        const float* ch[1] = {buf.data()};
        eng.pushBlock(ch, 1, kN);
        eng.computeFrame();
        const auto& db = eng.binDb();
        for (int k = 0; k < AnalyzerEngine::kNumBins; ++k)
            acc[static_cast<std::size_t>(k)] += db[static_cast<std::size_t>(k)];
    }
    std::vector<float> avg(static_cast<std::size_t>(AnalyzerEngine::kNumBins));
    for (int k = 0; k < AnalyzerEngine::kNumBins; ++k)
        avg[static_cast<std::size_t>(k)] = static_cast<float>(acc[static_cast<std::size_t>(k)] / runs);
    return avg;
}

void tiltWhite() {
    std::printf("Tilt (white noise, flat per-bin power):\n");
    const auto avg = averageSpectrum(/*pink=*/false, 40);
    const int k0 = static_cast<int>(std::ceil(100.0 / kBinHz));   // 100 Hz
    const int k1 = static_cast<int>(std::floor(10000.0 / kBinHz));  // 10 kHz
    const double s = slope(avg, k0, k1);
    std::printf("  measured slope = %.3f dB/oct (want ~%.1f)\n", s, AnalyzerEngine::kTiltDbPerOct);
    check(std::abs(s - AnalyzerEngine::kTiltDbPerOct) < 0.7, "white reads +tilt dB/oct");
}

void tiltPink() {
    std::printf("Pink (-3 dB/oct per-bin power):\n");
    const auto avg = averageSpectrum(/*pink=*/true, 40);
    const int k0 = static_cast<int>(std::ceil(100.0 / kBinHz));
    const int k1 = static_cast<int>(std::floor(10000.0 / kBinHz));
    const double s = slope(avg, k0, k1);
    const double expected = AnalyzerEngine::kTiltDbPerOct - 3.0;  // pink natural slope -3
    std::printf("  measured slope = %.3f dB/oct (tilt-3 = %.1f; flat only if tilt were 3)\n", s, expected);
    check(std::abs(s - expected) < 0.9, "pink reads (tilt - 3) dB/oct");
}

}  // namespace

int main() {
    twoTone();
    tiltWhite();
    tiltPink();
    std::printf("\n%s\n", failures == 0 ? "ALL ANALYZER TESTS PASS" : "SOME ANALYZER TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
