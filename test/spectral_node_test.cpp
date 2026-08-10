// Offline verification of SpectralNode / SpectralFft against the VERIFIED proto
// pipeline (proto/spectral_stft.py, spectral_core.py, spectral_ballistics.py),
// via fixtures dumped by proto/dump_spectral_fixtures.py. Numbered PASS/FAIL like
// compressor_node_test.cpp; exits nonzero on any failure.
//
//   1. FFT round trip (<1e-12) and forward vs numpy known-vector (<1e-9).
//   2. Latency == N exactly (impulse: first nonzero at N; xcorr delay == 1024).
//   3. Chunk-size invariance: 64/128/480/1000 and one-shot -> BIT-IDENTICAL.
//   4. depth=0 passthrough: output == N-shifted input (<1e-9).
//   5. Full chain: C++ reproduces each proto fixture (measured tol, gate ~10x).
//   6. Stereo-link sanity: L==R gives per-channel output == the mono result.
//
// Plain C++ console target, no JUCE (same rationale as CompressorNodeTest).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "dsp/SpectralNode.h"
#include "spectral_fixtures.h"

namespace {

bool gAllPass = true;

void check(const char* tag, const char* name, bool cond, const char* detail) {
    if (!cond) gAllPass = false;
    std::printf("[%s] %s: %s -- %s\n", tag, cond ? "PASS" : "FAIL", name, detail);
}

constexpr int N = SpectralNode::kFftSize;   // 1024
constexpr int HOP = SpectralNode::kHop;     // 256

// Feed a mono signal through a fresh node and return its (N-delayed) output,
// padded with enough trailing zeros to flush every aligned output sample. The
// returned vector has length len + pad; the caller aligns via [N : N+len].
std::vector<float> runMono(const SpectralNode::Params& p, const std::vector<float>& in,
                           int chunk, double fs = specfix::kFs) {
    const int len = static_cast<int>(in.size());
    // Round len up to a hop boundary, then a full frame of latency (mirrors the
    // proto flush: pad_to_hop + latency).
    const int pad = ((HOP - len % HOP) % HOP) + N;
    const int total = len + pad;
    std::vector<float> buf(static_cast<std::size_t>(total), 0.0f);
    for (int i = 0; i < len; ++i) buf[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)];

    SpectralNode node;
    node.prepare(fs, chunk > 0 ? chunk : total, 1);
    node.setParams(p);

    std::vector<float> out(static_cast<std::size_t>(total), 0.0f);
    const int step = chunk > 0 ? chunk : total;
    for (int i = 0; i < total; i += step) {
        const int n = std::min(step, total - i);
        float* ch[1] = {out.data() + i};
        // copy the input slice into the output-in-place buffer
        for (int k = 0; k < n; ++k)
            out[static_cast<std::size_t>(i + k)] = buf[static_cast<std::size_t>(i + k)];
        node.process(ch, 1, n);
    }
    return out;
}

// ============================================================
// 1: FFT round trip + forward vs numpy
// ============================================================
void verifyFft() {
    SpectralFft fft;
    fft.prepare(N);
    constexpr int nb = N / 2 + 1;

    double maxFwdErr = 0.0, maxRtErr = 0.0;
    std::vector<double> re(static_cast<std::size_t>(nb)), im(static_cast<std::size_t>(nb));
    std::vector<double> back(static_cast<std::size_t>(N));

    for (int f = 0; f < specfix::kNumFftFixtures; ++f) {
        const auto& fx = specfix::kFftFixtures[f];
        fft.forward(fx.x.data(), re.data(), im.data());
        for (int b = 0; b < nb; ++b) {
            maxFwdErr = std::max(maxFwdErr, std::fabs(re[static_cast<std::size_t>(b)] - fx.re[static_cast<std::size_t>(b)]));
            maxFwdErr = std::max(maxFwdErr, std::fabs(im[static_cast<std::size_t>(b)] - fx.im[static_cast<std::size_t>(b)]));
        }
        fft.inverse(re.data(), im.data(), back.data());
        for (int n = 0; n < N; ++n)
            maxRtErr = std::max(maxRtErr, std::fabs(back[static_cast<std::size_t>(n)] - fx.x[static_cast<std::size_t>(n)]));
    }
    char d[160];
    std::snprintf(d, sizeof d, "max |forward - numpy rfft| = %.3e over %d vectors", maxFwdErr, specfix::kNumFftFixtures);
    check("fft", "forward matches numpy rfft (<1e-9)", maxFwdErr < 1e-9, d);
    std::snprintf(d, sizeof d, "max |irfft(rfft(x)) - x| = %.3e", maxRtErr);
    check("fft", "round trip is identity (<1e-12)", maxRtErr < 1e-12, d);
}

// ============================================================
// 2: latency == N exactly
// ============================================================
void verifyLatency() {
    SpectralNode::Params p; p.depth = 0.0;  // identity path: cleanest latency probe
    // impulse at sample 0
    std::vector<float> imp(3000, 0.0f);
    imp[0] = 1.0f;
    std::vector<float> out = runMono(p, imp, 0);

    // first nonzero output sample
    int firstNonzero = -1;
    for (int i = 0; i < static_cast<int>(out.size()); ++i)
        if (std::fabs(out[static_cast<std::size_t>(i)]) > 1e-9) { firstNonzero = i; break; }
    char d[160];
    std::snprintf(d, sizeof d, "first nonzero output at sample %d (expected %d)", firstNonzero, N);
    check("latency", "impulse: first nonzero output at sample N", firstNonzero == N, d);

    // cross-correlation delay against a random signal
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> x(4000);
    for (auto& v : x) v = dist(rng);
    std::vector<float> y = runMono(p, x, 0);

    double best = -1e300; int bestLag = -1;
    for (int lag = 0; lag <= 2 * N; ++lag) {
        double corr = 0.0;
        for (int i = 0; i < static_cast<int>(x.size()); ++i) {
            const int j = i + lag;
            if (j < static_cast<int>(y.size()))
                corr += static_cast<double>(x[static_cast<std::size_t>(i)]) * static_cast<double>(y[static_cast<std::size_t>(j)]);
        }
        if (corr > best) { best = corr; bestLag = lag; }
    }
    std::snprintf(d, sizeof d, "cross-correlation peak lag = %d (expected %d)", bestLag, N);
    check("latency", "xcorr delay == 1024", bestLag == N, d);
}

// ============================================================
// 3: chunk-size invariance (BIT-IDENTICAL)
// ============================================================
void verifyChunkInvariance() {
    SpectralNode::Params p;  // depth=5 default: exercises the full active chain
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> dist(-0.4f, 0.4f);
    std::vector<float> x(9000);
    for (auto& v : x) v = dist(rng);

    std::vector<float> ref = runMono(p, x, 0);  // one shot
    const int chunks[] = {64, 128, 480, 1000};
    double worst = 0.0;
    bool allExact = true;
    for (int c : chunks) {
        std::vector<float> out = runMono(p, x, c);
        double maxDiff = 0.0;
        for (std::size_t i = 0; i < ref.size(); ++i)
            maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(out[i]) - static_cast<double>(ref[i])));
        worst = std::max(worst, maxDiff);
        if (maxDiff != 0.0) allExact = false;
    }
    char d[160];
    std::snprintf(d, sizeof d, "worst |chunked - one-shot| over 64/128/480/1000 = %.3e (gate ==0)", worst);
    check("chunk", "chunked output bit-identical to one-shot", allExact, d);
}

// ============================================================
// 4: depth=0 passthrough (N-shifted identity)
// ============================================================
void verifyPassthrough() {
    SpectralNode::Params p; p.depth = 0.0;
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> dist(-0.6f, 0.6f);
    std::vector<float> x(5000);
    for (auto& v : x) v = dist(rng);
    std::vector<float> y = runMono(p, x, 0);

    double maxErr = 0.0;
    for (int i = 0; i < static_cast<int>(x.size()); ++i)
        maxErr = std::max(maxErr, std::fabs(static_cast<double>(y[static_cast<std::size_t>(i + N)]) - static_cast<double>(x[static_cast<std::size_t>(i)])));
    char d[160];
    std::snprintf(d, sizeof d, "max |out[N+i] - in[i]| = %.3e (gate <1e-9)", maxErr);
    check("passthrough", "depth=0 output == N-shifted input", maxErr < 1e-9, d);
}

// ============================================================
// 5: full chain vs proto fixtures
// ============================================================
void verifyFullChain() {
    double globalWorst = 0.0;
    for (int f = 0; f < specfix::kNumChainFixtures; ++f) {
        const auto& fx = specfix::kChainFixtures[f];
        const int len = fx.len;
        std::vector<float> in(static_cast<std::size_t>(len));
        for (int i = 0; i < len; ++i) in[static_cast<std::size_t>(i)] = static_cast<float>(fx.x[static_cast<std::size_t>(i)]);

        SpectralNode::Params p;
        p.depth = fx.depth; p.detail = fx.detail; p.attackMs = fx.attackMs; p.releaseMs = fx.releaseMs;
        std::vector<float> out = runMono(p, in, 0);

        // cppOut[N : N+len] aligns with fixture.out[0:len].
        double maxDiff = 0.0;
        for (int i = 0; i < len; ++i) {
            const double c = static_cast<double>(out[static_cast<std::size_t>(i + N)]);
            const double r = fx.out[static_cast<std::size_t>(i)];
            maxDiff = std::max(maxDiff, std::fabs(c - r));
        }
        globalWorst = std::max(globalWorst, maxDiff);
        char d[192];
        std::snprintf(d, sizeof d, "%-18s max|C++ - proto| = %.3e", fx.name, maxDiff);
        // Per-fixture gate. The ONLY divergence source is the C++ radix-2 FFT vs
        // numpy pocketfft rounding (the median/interp/ballistics port is exact),
        // measured ~1e-8 worst; the gate is ~10x that. Identity (depth=0) has no
        // gain path at all, so it must be near-exact (FFT round-trip only).
        const double gate = (fx.depth == 0.0) ? 1e-9 : 1e-7;
        check("chain", d, maxDiff < gate, fx.depth == 0.0 ? "identity gate 1e-9" : "active gate 1e-7 (~10x measured FFT-rounding floor)");
    }
    char d[160];
    std::snprintf(d, sizeof d, "worst over all fixtures = %.3e", globalWorst);
    check("chain", "all fixtures reproduced", globalWorst < 1e-7, d);
}

// ============================================================
// 6: stereo-link sanity (L==R reproduces the mono result)
// ============================================================
void verifyStereo() {
    SpectralNode::Params p; p.depth = 6.0; p.detail = 4.0;
    std::mt19937 rng(13);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    const int len = 4000;
    std::vector<float> x(static_cast<std::size_t>(len));
    for (auto& v : x) v = dist(rng);

    // mono reference
    std::vector<float> mono = runMono(p, x, 0);

    // stereo run with L==R, one shot
    const int pad = ((HOP - len % HOP) % HOP) + N;
    const int total = len + pad;
    std::vector<float> l(static_cast<std::size_t>(total), 0.0f), r(static_cast<std::size_t>(total), 0.0f);
    for (int i = 0; i < len; ++i) { l[static_cast<std::size_t>(i)] = x[static_cast<std::size_t>(i)]; r[static_cast<std::size_t>(i)] = x[static_cast<std::size_t>(i)]; }
    SpectralNode node;
    node.prepare(specfix::kFs, total, 2);
    node.setParams(p);
    float* ch[2] = {l.data(), r.data()};
    node.process(ch, 2, total);

    double maxLR = 0.0, maxLMono = 0.0;
    for (int i = 0; i < total; ++i) {
        maxLR = std::max(maxLR, std::fabs(static_cast<double>(l[static_cast<std::size_t>(i)]) - static_cast<double>(r[static_cast<std::size_t>(i)])));
        maxLMono = std::max(maxLMono, std::fabs(static_cast<double>(l[static_cast<std::size_t>(i)]) - static_cast<double>(mono[static_cast<std::size_t>(i)])));
    }
    char d[192];
    std::snprintf(d, sizeof d, "max |L - R| = %.3e (shared max-gain applied to both channels)", maxLR);
    check("stereo", "L==R stays identical across channels", maxLR == 0.0, d);
    std::snprintf(d, sizeof d, "max |L - mono| = %.3e (linked map reduces to the mono result)", maxLMono);
    check("stereo", "L==R reproduces the mono reference", maxLMono == 0.0, d);
}

}  // namespace

int main() {
    std::printf("=== 1. FFT ===\n");
    verifyFft();
    std::printf("\n=== 2. Latency ===\n");
    verifyLatency();
    std::printf("\n=== 3. Chunk-size invariance ===\n");
    verifyChunkInvariance();
    std::printf("\n=== 4. depth=0 passthrough ===\n");
    verifyPassthrough();
    std::printf("\n=== 5. Full chain vs proto ===\n");
    verifyFullChain();
    std::printf("\n=== 6. Stereo link ===\n");
    verifyStereo();

    std::printf("\n%s\n", gAllPass ? "ALL PASS" : "SOME FAILURES");
    return gAllPass ? 0 : 1;
}
