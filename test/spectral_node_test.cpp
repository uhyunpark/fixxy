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
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <new>
#include <random>
#include <vector>

#include "dsp/SpectralNode.h"
#include "spectral_fixtures.h"
#include "spectral_hard_wola_fixtures.h"
#include "spectral_sidechain_fixtures.h"

static bool gTrackHardHeap = false;
static std::size_t gHardAllocations = 0, gHardDeletions = 0;
void* operator new(std::size_t n) {
    if (gTrackHardHeap) ++gHardAllocations;
    if (auto* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if (gTrackHardHeap) ++gHardDeletions; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

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
                           int chunk, double fs = specfix::kFs, int preparedMax = 0,
                           int detectionMode = 0) {
    const int len = static_cast<int>(in.size());
    // Round len up to a hop boundary, then a full frame of latency (mirrors the
    // proto flush: pad_to_hop + latency).
    const int pad = ((HOP - len % HOP) % HOP) + N;
    const int total = len + pad;
    std::vector<float> buf(static_cast<std::size_t>(total), 0.0f);
    for (int i = 0; i < len; ++i) buf[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)];

    SpectralNode node;
    node.prepare(fs, preparedMax > 0 ? preparedMax : (chunk > 0 ? chunk : total), 1);
    node.setParams(p);
    node.setDetectionMode(detectionMode);

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

// ============================================================
// 7: weight-curve chain vs proto fixtures (boost / suppress / gate)
// ============================================================
void verifyWeightChain() {
    double globalWorst = 0.0;
    for (int f = 0; f < specfix::kNumWeightChainFixtures; ++f) {
        const auto& fx = specfix::kWeightChainFixtures[f];
        const int len = fx.len;
        std::vector<float> in(static_cast<std::size_t>(len));
        for (int i = 0; i < len; ++i) in[static_cast<std::size_t>(i)] = static_cast<float>(fx.x[static_cast<std::size_t>(i)]);

        SpectralNode::Params p;
        p.depth = fx.depth; p.detail = fx.detail; p.attackMs = fx.attackMs; p.releaseMs = fx.releaseMs;
        p.rangeLoHz = fx.rangeLoHz; p.rangeHiHz = fx.rangeHiHz;
        for (int b = 0; b < specfix::kMaxWeightBands; ++b) {
            const auto& wb = fx.bands[static_cast<std::size_t>(b)];
            p.bands[static_cast<std::size_t>(b)] = {wb.on, wb.type, wb.freqHz, wb.gainDb, wb.q};
        }
        std::vector<float> out = runMono(p, in, 0);

        double maxDiff = 0.0;
        for (int i = 0; i < len; ++i) {
            const double c = static_cast<double>(out[static_cast<std::size_t>(i + N)]);
            const double r = fx.out[static_cast<std::size_t>(i)];
            maxDiff = std::max(maxDiff, std::fabs(c - r));
        }
        globalWorst = std::max(globalWorst, maxDiff);
        char d[192];
        std::snprintf(d, sizeof d, "%-20s max|C++ - proto| = %.3e", fx.name, maxDiff);
        // Same divergence source as the base chain (radix-2 vs pocketfft FFT
        // rounding) plus the closed-form weight table (numpy vs C++ double);
        // measured worst ~1e-9, gate ~10x.
        check("weight", d, maxDiff < 1.2e-8, "weight gate 1.2e-8 (~10x measured FFT/weight-rounding floor)");
    }
    char d[160];
    std::snprintf(d, sizeof d, "worst over all weight fixtures = %.3e", globalWorst);
    check("weight", "all weight fixtures reproduced", globalWorst < 1.2e-8, d);
}

// ============================================================
// 8: SIDEBAR chain vs proto fixtures (max_cut / wet_trim / M-S / link /
//    focus / tilts). Stereo. proto/spectral_sidebar.py run_sidebar applies
//    wet_trim as a flat post-gain on the wet output; the NODE does not (that
//    is the plugin's SpectralMix job), so we replicate the trim here to match
//    the fixture, exactly as the plugin will.
// ============================================================
void verifySidebar() {
    double globalWorst = 0.0;
    for (int f = 0; f < specfix::kNumSidebarFixtures; ++f) {
        const auto& fx = specfix::kSidebarFixtures[f];
        const int len = fx.len;

        // Feed a generous flush tail (3N of zeros) so the node fully reconstructs
        // every output position the batch reference reached (see the alignment note
        // below — the fixture lives 2N into the node's stream).
        const int total = len + 3 * N;
        std::vector<float> l(static_cast<std::size_t>(total), 0.0f), r(static_cast<std::size_t>(total), 0.0f);
        for (int i = 0; i < len; ++i) {
            l[static_cast<std::size_t>(i)] = static_cast<float>(fx.xL[static_cast<std::size_t>(i)]);
            r[static_cast<std::size_t>(i)] = static_cast<float>(fx.xR[static_cast<std::size_t>(i)]);
        }

        SpectralNode::Params p;
        p.depth = fx.depth; p.detail = fx.detail; p.attackMs = fx.attackMs; p.releaseMs = fx.releaseMs;
        p.maxCutDb = fx.maxCutDb; p.wetTrimDb = fx.wetTrimDb; p.msMode = fx.msMode;
        p.linkPct = fx.linkPct; p.focus = fx.focus;
        p.tiltAttack = fx.tiltAttack; p.tiltRelease = fx.tiltRelease; p.tiltDetail = fx.tiltDetail;

        SpectralNode node;
        node.prepare(specfix::kFs, total, 2);
        node.setParams(p);
        float* ch[2] = {l.data(), r.data()};
        node.process(ch, 2, total);

        // wet_trim: the plugin (SpectralMix) applies it; replicate to match the fixture.
        const double trim = std::pow(10.0, fx.wetTrimDb / 20.0);

        // ALIGNMENT — the sidebar fixture carries a KNOWN proto artifact. Its batch
        // harness (spectral_sidebar._stereo_hop_loop) builds a ZERO-latency out_A
        // (out_A[p] reconstructs input position p; verified: identity out_A[0:len] ==
        // input to 1e-16), but process_channels then returns out_A[N : N+len],
        // double-counting the N-sample STFT latency. So fixture[i] is the processed
        // reconstruction of input position (N + i). Our streaming node reports latency
        // N (out[j] = reconstruction of input position j - N), so that same
        // reconstruction is out[(N+i) + N] = out[i + 2N]. The proto's own gates only
        // compare sidebar-to-sidebar, where this shared shift cancels, so it never
        // surfaced there; we align to it explicitly rather than reproduce the bug.
        //
        // The harness also stops one flush short (pads by only pad_to_hop + N), so it
        // finalizes positions only up to (n_hops*hop - 2N + hop); the rest of its
        // window is never produced and reads 0 (last nonzero fixture sample 1279/2048).
        // We verify every sample it actually computed and stop where it stopped.
        constexpr int kOffset = 2 * N;  // fixture[i] == node out[i + 2N]
        const int padToHop = (HOP - len % HOP) % HOP;
        const int nHops = (len + padToHop + N) / HOP;
        int validAligned = nHops * HOP - 2 * N + HOP;
        if (validAligned > len) validAligned = len;
        if (validAligned < 0) validAligned = 0;

        double maxDiff = 0.0;
        for (int i = 0; i < validAligned; ++i) {
            const double cl = static_cast<double>(l[static_cast<std::size_t>(i + kOffset)]) * trim;
            const double cr = static_cast<double>(r[static_cast<std::size_t>(i + kOffset)]) * trim;
            maxDiff = std::max(maxDiff, std::fabs(cl - fx.outL[static_cast<std::size_t>(i)]));
            maxDiff = std::max(maxDiff, std::fabs(cr - fx.outR[static_cast<std::size_t>(i)]));
        }
        globalWorst = std::max(globalWorst, maxDiff);
        char d[192];
        std::snprintf(d, sizeof d, "%-24s max|C++ - proto| = %.3e over [0,%d)", fx.name, maxDiff, validAligned);
        // Same divergence source as the base/weight chains: radix-2 vs pocketfft
        // FFT rounding, now compounded across the 2-channel M/S + link algebra;
        // measured worst ~1e-8, gate ~10x.
        check("sidebar", d, maxDiff < 2e-8, "sidebar gate 2e-8 (~10x measured FFT-rounding floor)");
    }
    char d[160];
    std::snprintf(d, sizeof d, "worst over all sidebar fixtures = %.3e", globalWorst);
    check("sidebar", "all sidebar fixtures reproduced", globalWorst < 2e-8, d);
}

// A routing edit must not reinterpret the unfinished overlap-add history.
// At depth 0 either basis is identity, including while modes change mid-window.
void verifyMsModeTransitions() {
    constexpr int total = 16384;
    for (const int chunk : {64, 127, 256, 1000}) {
        std::vector<float> l(total), r(total), originalL(total), originalR(total);
        for (int i = 0; i < total; ++i) {
            const auto idx = static_cast<std::size_t>(i);
            originalL[idx] = l[idx] = static_cast<float>(0.4 * std::sin(0.072 * i));
            originalR[idx] = r[idx] = static_cast<float>(0.23 * std::cos(0.11 * i));
        }
        SpectralNode node;
        node.prepare(specfix::kFs, chunk, 2);
        SpectralNode::Params p;
        p.depth = 0.0;
        int block = 0;
        for (int i = 0; i < total; i += chunk, ++block) {
            p.msMode = (block / 7) % 2 != 0;
            node.setParams(p);
            float* ch[2] = {l.data() + i, r.data() + i};
            node.process(ch, 2, std::min(chunk, total - i));
        }
        double maxDiff = 0.0;
        for (int i = 0; i < total; ++i) {
            const auto idx = static_cast<std::size_t>(i);
            const double expectedL = i < N ? 0.0 : originalL[static_cast<std::size_t>(i - N)];
            const double expectedR = i < N ? 0.0 : originalR[static_cast<std::size_t>(i - N)];
            maxDiff = std::max(maxDiff, std::fabs(l[idx] - expectedL));
            maxDiff = std::max(maxDiff, std::fabs(r[idx] - expectedR));
        }
        char d[160];
        std::snprintf(d, sizeof d, "chunk %d, repeated L/R <-> M/S, max identity error %.3e", chunk, maxDiff);
        check("routing", "mode transitions preserve depth 0 identity", maxDiff < 1e-7, d);
    }
}

void verifyOversizedBlocks() {
    std::vector<float> input(8192);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(0.4 * std::sin(0.072 * static_cast<double>(i))
                                   + 0.1 * std::cos(0.231 * static_cast<double>(i)));
    for (const double depth : {0.0, 5.0}) {
        SpectralNode::Params p;
        p.depth = depth;
        const auto reference = runMono(p, input, 128);
        for (const int chunk : {512, 3001, 0}) {
            const auto actual = runMono(p, input, chunk, specfix::kFs, 128);
            double maxDiff = 0.0;
            for (std::size_t i = 0; i < reference.size(); ++i)
                maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(actual[i]) - reference[i]));
            char d[160];
            std::snprintf(d, sizeof d, "prepare 128, process %d (0 = one-shot), depth %.0f, max error %.3e",
                          chunk, depth, maxDiff);
            check("capacity", "oversized calls match bounded calls", maxDiff == 0.0, d);
        }
    }
}

void verifyActiveMsTransitions() {
    constexpr int total = 8192;
    constexpr int chunk = 127;  // Changes do not coincide with hop boundaries.
    for (const int numChannels : {1, 2}) {
        SpectralNode node, reference;
        node.prepare(specfix::kFs, chunk, numChannels);
        reference.prepare(specfix::kFs, chunk, numChannels);
        SpectralNode::Params p;  // Active default depth, attack, and release.
        reference.setParams(p);
        bool valid = true;
        double maxMagnitude = 0.0;
        double monoError = 0.0;
        for (int offset = 0, block = 0; offset < total; offset += chunk, ++block) {
            std::array<float, chunk> l{}, r{}, refL{}, refR{};
            const int count = std::min(chunk, total - offset);
            for (int i = 0; i < count; ++i) {
                const auto idx = static_cast<std::size_t>(i);
                l[idx] = refL[idx] = static_cast<float>(0.4 * std::sin(0.072 * (offset + i)));
                r[idx] = refR[idx] = static_cast<float>(0.23 * std::cos(0.11 * (offset + i)));
            }
            p.msMode = (block / 7) % 2 != 0;
            node.setParams(p);
            float* ch[2] = {l.data(), r.data()};
            float* ref[2] = {refL.data(), refR.data()};
            node.process(ch, numChannels, count);
            reference.process(ref, numChannels, count);
            for (int c = 0; c < numChannels; ++c)
                for (int i = 0; i < count; ++i) {
                    valid = valid && std::isfinite(ch[c][i]);
                    maxMagnitude = std::max(maxMagnitude, std::fabs(static_cast<double>(ch[c][i])));
                    if (numChannels == 1)
                        monoError = std::max(monoError, std::fabs(static_cast<double>(ch[c][i]) - ref[c][i]));
                }
        }
        char d[160];
        std::snprintf(d, sizeof d, "%d channels, peak %.3e, mono mode-toggle error %.3e",
                      numChannels, maxMagnitude, monoError);
        check("routing", "active transitions stay finite; mono ignores M/S", valid && maxMagnitude < 1.0 && monoError == 0.0, d);
    }
}

void verifyNewSoftFixture() {
    namespace hf = spectralhardfix;
    std::array<std::vector<float>, 2> buffers;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        buffers[ch].assign(hf::kOutputLength, 0.0f);
        for (std::size_t i = 0; i < hf::kInputLength; ++i)
            buffers[ch][i] = static_cast<float>(hf::kInput[ch*hf::kInputLength+i]);
    }
    SpectralNode node;
    node.prepare(hf::kSampleRate, 480, 2);
    constexpr std::array<std::size_t, 4> chunks{17, 480, 1, 129};
    for (std::size_t at = 0, block = 0; at < hf::kOutputLength; ++block) {
        const auto count = std::min(chunks[block%chunks.size()], hf::kOutputLength-at);
        float* channels[2]{buffers[0].data()+at, buffers[1].data()+at};
        node.process(channels, 2, static_cast<int>(count));
        at += count;
    }
    double error = 0.0;
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < hf::kOutputLength; ++i)
            error = std::max(error, std::abs(static_cast<double>(buffers[ch][i])-hf::kSoft[ch*hf::kOutputLength+i]));
    char detail[160];
    std::snprintf(detail, sizeof detail, "fresh sqrt-Hann WOLA fixture vs untouched legacy Soft max %.12g", error);
    check("hard-baseline", "new Soft fixture matches before Hard port", error < 1e-7, detail);
}

void verifyHardFixtures() {
    namespace hf = spectralhardfix;
    for (int mode : {0, 1, 2}) {
        std::array<std::vector<float>, 2> buffers;
        for (std::size_t ch = 0; ch < 2; ++ch) {
            buffers[ch].assign(hf::kOutputLength, 0.0f);
            for (std::size_t i = 0; i < hf::kInputLength; ++i)
                buffers[ch][i] = static_cast<float>(hf::kInput[ch*hf::kInputLength+i]);
        }
        SpectralNode node;
        node.prepare(hf::kSampleRate, 480, 2);
        node.setDetectionMode(mode == 1 ? 1 : 0);
        constexpr std::array<std::size_t, 4> chunks{17, 480, 1, 129};
        // Set the latest requested mode just before the sample completing the
        // specified hop. A setter itself must neither reset state nor run FFTs.
        constexpr std::size_t hardAt = hf::kSwitchHardAtInputSample-1;
        constexpr std::size_t softAt = hf::kSwitchSoftAtInputSample-1;
        for (std::size_t at = 0, block = 0; at < hf::kOutputLength; ++block) {
            gTrackHardHeap = true;
            if (mode == 2 && at == hardAt) node.setDetectionMode(1);
            if (mode == 2 && at == softAt) node.setDetectionMode(0);
            auto count = std::min(chunks[block%chunks.size()], hf::kOutputLength-at);
            if (mode == 2 && at < hardAt) count = std::min(count, hardAt-at);
            if (mode == 2 && at < softAt) count = std::min(count, softAt-at);
            float* channels[2]{buffers[0].data()+at, buffers[1].data()+at};
            node.process(channels, 2, static_cast<int>(count));
            gTrackHardHeap = false;
            at += count;
        }
        const auto& expected = mode == 0 ? hf::kSoft : mode == 1 ? hf::kHard : hf::kSwitch;
        double error = 0.0;
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t i = 0; i < hf::kOutputLength; ++i)
                error = std::max(error, std::abs(static_cast<double>(buffers[ch][i])-expected[ch*hf::kOutputLength+i]));
        char detail[160];
        std::snprintf(detail, sizeof detail, "mode %d generated audio fixture max %.12g", mode, error);
        check("hard-fixture", "Soft/Hard/switch with retained envelope", error < 1e-7, detail);
    }
}

SpectralNode::Params hardCaseParams(int scenario) {
    SpectralNode::Params p;
    if (scenario == 1) p.msMode = true; // mono guard
    if (scenario == 2) { p.detail = 10; p.msMode = true; p.linkPct = 0; }
    if (scenario == 3) {
        p.detail = 2; p.focus = .4; p.linkPct = 50; p.maxCutDb = 6;
        p.tiltDetail = -.5; p.tiltAttack = .5; p.tiltRelease = -.3;
        p.rangeLoHz = 200; p.rangeHiHz = 8000;
        p.bands[0].on = true; p.bands[0].type = 0;
        p.bands[0].freqHz = 3000; p.bands[0].gainDb = 12; p.bands[0].q = .7;
    }
    if (scenario == 4) { p.focus = 1; p.linkPct = 50; }
    if (scenario == 5) { p.focus = 1; p.msMode = true; }
    if (scenario == 6) { p.focus = -1; p.linkPct = 0; p.msMode = true; }
    return p;
}

std::array<std::vector<float>, 2> runHardCase(int scenario, int mode, bool irregular) {
    namespace hf = spectralhardfix;
    std::array<std::vector<float>, 2> buffers;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        buffers[ch].assign(hf::kOutputLength, 0.0f);
        for (std::size_t i = 0; i < hf::kInputLength; ++i)
            buffers[ch][i] = static_cast<float>(hf::kInput[ch*hf::kInputLength+i]);
    }
    const int channels = scenario == 1 || scenario == 5 ? 1 : 2;
    SpectralNode node; node.prepare(hf::kSampleRate, 480, channels);
    gTrackHardHeap = true;
    node.setParams(hardCaseParams(scenario)); node.setDetectionMode(mode);
    constexpr std::array<std::size_t, 4> chunks{17, 480, 1, 129};
    for (std::size_t at = 0, block = 0; at < hf::kOutputLength; ++block) {
        const auto count = std::min(irregular ? chunks[block%chunks.size()] : hf::kOutputLength, hf::kOutputLength-at);
        float* data[2]{buffers[0].data()+at, buffers[1].data()+at};
        node.process(data, channels, static_cast<int>(count));
        at += count;
    }
    gTrackHardHeap = false;
    return buffers;
}

void verifyHardRouting() {
    for (int scenario = 0; scenario < 7; ++scenario) {
        const auto irregular = runHardCase(scenario, 1, true);
        const auto batch = runHardCase(scenario, 1, false);
        const bool exact = irregular == batch;
        char detail[120]; std::snprintf(detail, sizeof detail, "routing scenario %d", scenario);
        check("hard-routing", "mono/stereo/MS/focus/link/weight chunk identity", exact, detail);
    }
    char detail[120];
    std::snprintf(detail, sizeof detail, "prepared setters/process new/delete=%zu/%zu", gHardAllocations, gHardDeletions);
    check("hard-thread", "prepared operations allocate/delete nothing", gHardAllocations == 0 && gHardDeletions == 0, detail);
}

double fittedAmplitude(const std::vector<float>& output, std::size_t start,
                       std::size_t count, double frequency, double rate) {
    double ss = 0, cc = 0, sc = 0, sy = 0, cy = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const double phase = 2.0*3.14159265358979323846*frequency*static_cast<double>(i)/rate;
        const double s = std::sin(phase), c = std::cos(phase), y = output[start+i];
        ss += s*s; cc += c*c; sc += s*c; sy += s*y; cy += c*y;
    }
    const double determinant = ss*cc-sc*sc;
    return std::hypot((sy*cc-cy*sc)/determinant, (cy*ss-sy*sc)/determinant);
}

void verifyHardTransfer() {
    constexpr std::size_t length = 48000;
    constexpr double rate = 48000.0, pi = 3.14159265358979323846;
    for (double offset : {0.0, .5}) {
        const double frequency = (64.0+offset)*rate/N;
        double previous = -1e3;
        for (double level : {-60.0, -36.0, -24.0, -12.0, -6.0}) {
            const double amplitude = std::pow(10.0, level/20.0);
            std::vector<float> source(length);
            for (std::size_t i = 0; i < length; ++i)
                source[i] = static_cast<float>(amplitude*std::sin(2*pi*frequency*static_cast<double>(i)/rate+.3));
            const auto output = runMono({}, source, 137, rate, 128, 1);
            const double attenuation = 20*std::log10(amplitude/fittedAmplitude(output, N+length/2, length/2, frequency, rate));
            char detail[160];
            std::snprintf(detail, sizeof detail, "bin offset %.1f, input %.0f dBFS, attenuation %.9f dB", offset, level, attenuation);
            check("hard-level", "absolute level increases measured audio attenuation", attenuation >= previous-1e-6, detail);
            previous = attenuation;
        }
        previous = -1e3;
        std::vector<float> source(length);
        for (std::size_t i = 0; i < length; ++i)
            source[i] = static_cast<float>(.25*std::sin(2*pi*frequency*static_cast<double>(i)/rate));
        for (double depth : {0.0, 1.0, 3.0, 5.0, 7.0, 10.0}) {
            SpectralNode::Params p; p.depth = depth;
            const auto output = runMono(p, source, 137, rate, 128, 1);
            const double attenuation = 20*std::log10(.25/fittedAmplitude(output, N+length/2, length/2, frequency, rate));
            char detail[160];
            std::snprintf(detail, sizeof detail, "bin offset %.1f, depth %.0f, attenuation %.9f dB", offset, depth, attenuation);
            check("hard-depth", "depth increases measured audio attenuation", attenuation >= previous-1e-6, detail);
            if (depth == 0.0) {
                const auto legacy = runMono(p, source, 137, rate, 128, 0);
                double error = 0.0;
                for (std::size_t i = 0; i < length; ++i)
                    error = std::max(error, std::abs(static_cast<double>(output[N+i])-source[i]));
                std::snprintf(detail, sizeof detail, "Hard/Soft exact equality %d; max delayed input error %.12g; existing null gate 1e-9", output == legacy, error);
                check("hard-depth", "zero depth preserves legacy null and latency", output == legacy && error < 1e-9, detail);
            }
            previous = attenuation;
        }
    }
    SpectralNode::Params base;
    std::vector<float> source(8192);
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<float>(.3*std::sin(.393*static_cast<double>(i)));
    const auto noTilt = runMono(base, source, 137, rate, 128, 1);
    base.tiltDetail = 1.0;
    const auto tilt = runMono(base, source, 137, rate, 128, 1);
    check("hard-detail", "legacy Detail strength tilt has no Hard effect", noTilt == tilt, "width and timing remain legacy; only strength tilt ignored");
}

void verifyHardCapsAndSilence() {
    SpectralNode node; node.prepare(48000, 256, 1); node.setDetectionMode(1);
    SpectralNode::Params p; p.depth = 10; p.attackMs = 1;
    node.setParams(p);
    std::array<float, 256> block{};
    std::array<float, SpectralNode::kNumBins> red{};
    auto hop = [&](bool silent) {
        for (std::size_t i = 0; i < block.size(); ++i)
            block[i] = silent ? 0.0f : static_cast<float>(.9*std::sin(2*3.14159265358979323846*64*static_cast<double>(i)/1024.0));
        float* channel[]{block.data()}; node.process(channel, 1, 256);
        node.copyReductionDb(red.data(), SpectralNode::kNumBins);
        return static_cast<double>(*std::max_element(red.begin(), red.end()));
    };
    double before = 0.0;
    for (int i = 0; i < 200; ++i) before = hop(false);
    check("hard-cap", "global Hard ceiling stays within 40 dB", before <= 40.0+1e-5 && before > 6.0, "MaxCut24 retains legacy Off meaning");
    p.maxCutDb = 6.0; node.setParams(p);
    const double initial = hop(false);
    check("hard-cap", "lower cap releases old state rather than instant post-clamp", initial > 6.0, "legacy clamp precedes spatial smoothing and ballistics");
    double final = 0.0;
    for (int i = 0; i < 500; ++i) final = hop(false);
    check("hard-cap", "fixed cap settles below 6 dB", final <= 6.0+1e-5, "existing timing semantics preserved");
    for (int i = 0; i < 400; ++i) final = hop(true);
    check("hard-silence", "silence releases all previous reduction", final < 1e-8, "no new Hard demand on all-zero detector");
    const auto output = runMono({}, std::vector<float>(4096, 0.0f), 137, 48000, 128, 1);
    check("hard-silence", "fresh silence exact zero", std::all_of(output.begin(), output.end(), [](float x){return x == 0;}), "finite zero output");
}

void verifyHardCalibrationAndWidth() {
    constexpr double pi = 3.14159265358979323846;
    std::array<double, N> window{}, source{};
    double windowSum = 0.0;
    for (int n = 0; n < N; ++n) {
        window[static_cast<std::size_t>(n)] = std::sqrt(.5-.5*std::cos(2*pi*n/N));
        windowSum += window[static_cast<std::size_t>(n)];
    }
    check("hard-calibration", "actual sqrt-Hann analysis window sum", std::abs(windowSum-spectralhardfix::kAnalysisWindowSum) < 1e-10, "analysis amplitude calibration is independent of COLA norm");
    std::array<float, SpectralNode::kNumBins> broad{}, narrow{};
    for (int tone : {0, 64, N/2}) {
        for (int n = 0; n < N; ++n)
            source[static_cast<std::size_t>(n)] = static_cast<float>(tone == 0 ? .25 : tone == N/2 ? (n%2 == 0 ? .25 : -.25) : .25*std::sin(2*pi*tone*n/N));
        std::array<double, SpectralNode::kNumBins> raw{};
        for (int bin = 0; bin < SpectralNode::kNumBins; ++bin) {
            double re = 0.0, im = 0.0;
            for (int n = 0; n < N; ++n) {
                const double phase = 2*pi*bin*n/N;
                const double x = source[static_cast<std::size_t>(n)]*window[static_cast<std::size_t>(n)];
                re += x*std::cos(phase); im -= x*std::sin(phase);
            }
            const double amplitude = std::hypot(re, im)/windowSum*((bin == 0 || bin == N/2) ? 1.0 : 2.0);
            // Direct DFT plus the verified piecewise quadratic knee, independent
            // of the production FFT/calibration/knee helper and smoothed state.
            const double z = 20*std::log10(std::max(amplitude, 1e-12))+36;
            const double knee = z <= -3 ? 0 : z >= 3 ? z : (z+3)*(z+3)/12;
            raw[static_cast<std::size_t>(bin)] = std::min(40.0, .75*knee);
        }
        for (double detail : {0.0, 10.0}) {
            SpectralNode node; node.prepare(48000, 256, 1);
            SpectralNode::Params p; p.detail = detail; p.attackMs = 1; p.releaseMs = 1;
            node.setParams(p); node.setDetectionMode(1);
            std::array<float, 256> block{};
            for (int hop = 0; hop < 64; ++hop) {
                for (std::size_t n = 0; n < block.size(); ++n) block[n] = static_cast<float>(source[n]);
                float* ch[]{block.data()}; node.process(ch, 1, 256);
            }
            std::array<float, SpectralNode::kNumBins> red{};
            node.copyReductionDb(red.data(), SpectralNode::kNumBins);
            const double width = detail == 0 ? SpectralNode::kW0 : SpectralNode::kW10;
            const double ratio = std::pow(2.0, width*.5);
            double error = 0.0;
            for (int bin = 0; bin < SpectralNode::kNumBins; ++bin) {
                // Legacy spectral_core._octave_bin_windows rounds OUTWARD;
                // this is deliberately not the newer control-model interval.
                const int lo = bin == 0 ? 0 : static_cast<int>(std::floor(bin/ratio));
                const int hi = std::min(N/2, static_cast<int>(std::ceil(bin*ratio)));
                double expected = 0.0;
                for (int k = lo; k <= hi; ++k) expected += raw[static_cast<std::size_t>(k)]/(hi-lo+1);
                error = std::max(error, std::abs(red[static_cast<std::size_t>(bin)]-expected));
            }
            char text[128]; std::snprintf(text, sizeof text, "tone bin %d, Detail %.0f, direct DFT/box max %.12g dB", tone, detail, error);
            check("hard-calibration", "integrated DC/interior/Nyquist and Detail width", error < 1e-5, text);
            if (tone == 64) { if (detail == 0) broad = red; else narrow = red; }
        }
    }
    check("hard-detail", "main Detail changes Hard frequency width", std::abs(broad[64]-narrow[64]) > 1.0f, "shared raw Hard sensitivity; independently checked width endpoints");
}

void verifyExternalSidechain() {
    namespace fx = spectralsidechainfix;
    using Audio = std::array<std::vector<float>, 2>;
    double frameGainWorst = 0.0;
    const auto run = [&frameGainWorst](int scenario, int chunk, bool external = true) {
        Audio audio, key;
        for (int ch = 0; ch < 2; ++ch) {
            audio[static_cast<std::size_t>(ch)].resize(fx::kOutputLength);
            key[static_cast<std::size_t>(ch)].resize(fx::kOutputLength);
            for (std::size_t n = 0; n < fx::kInputLength; ++n) {
                audio[static_cast<std::size_t>(ch)][n] = static_cast<float>(fx::kMain[static_cast<std::size_t>(ch)*fx::kInputLength+n]);
                key[static_cast<std::size_t>(ch)][n] = static_cast<float>(fx::kKey[static_cast<std::size_t>(ch)*fx::kInputLength+n]);
            }
        }
        SpectralNode node;
        const int mainChannels = scenario == 3 ? 1 : 2;
        node.prepare(fx::kSampleRate, 97, mainChannels);
        SpectralNode::Params params;
        if (scenario == 2) { params.msMode = true; params.linkPct = 0; }
        node.setParams(params);
        node.setDetectionMode(scenario >= 1 && scenario <= 3 ? 1 : 0);
        node.setExternalSidechainEnabled(external && scenario != 5);
        for (int pos = 0; pos < static_cast<int>(fx::kOutputLength);) {
            int count = std::min(chunk, static_cast<int>(fx::kOutputLength)-pos);
            int keyChannels = scenario == 2 ? 1 : scenario == 4 ? 0 : 2;
            if (scenario == 5) {
                for (int boundary : {512, 768, 1024, 1280})
                    if (pos < boundary) count = std::min(count, boundary-pos);
                keyChannels = pos < 512 ? 2 : pos < 768 ? 0 : pos < 1024 ? 1 : 2;
                // Control events label the COMPLETED hop, unlike connection
                // events which label its first new input sample.
                node.setExternalSidechainEnabled(pos >= 512 && pos < 1024);
                node.setDetectionMode(pos >= 512 && pos < 1024 ? 1 : 0);
            }
            if (scenario == 6) {
                for (int boundary : {333, 777, 1051})
                    if (pos < boundary) count = std::min(count, boundary-pos);
                keyChannels = pos < 333 ? 2 : pos < 777 ? 0 : pos < 1051 ? 1 : 2;
            }
            float* main[]{audio[0].data()+pos, audio[1].data()+pos};
            const float* keys[]{key[0].data()+pos, key[1].data()+pos};
            gTrackHardHeap = true;
            node.process(main, mainChannels, count, keys, keyChannels);
            gTrackHardHeap = false;
            pos += count;
            if (external && scenario < 2 && pos == 1280) {
                std::array<float, SpectralNode::kNumBins> reduction{};
                node.copyReductionDb(reduction.data(), SpectralNode::kNumBins);
                const auto& expected = scenario == 0 ? fx::kSoftGainFrame4 : fx::kHardGainFrame4;
                for (std::size_t bin = 0; bin < expected.size(); ++bin)
                    frameGainWorst = std::max(frameGainWorst,
                        std::abs(std::pow(10.0, -reduction[bin]/20.0)-expected[bin]));
            }
        }
        return audio;
    };
    gHardAllocations = gHardDeletions = 0;
    const std::array<const double*, 6> expected{
        fx::kSoft.data(), fx::kHard.data(), fx::kMonoKeyMs.data(),
        fx::kStereoKeyMono.data(), fx::kMissing.data(), fx::kSwitch.data()};
    double worst = 0.0;
    bool chunksExact = true;
    for (int scenario = 0; scenario < 6; ++scenario) {
        double scenarioWorst = 0.0;
        const auto reference = run(scenario, 1);
        const int channels = scenario == 3 ? 1 : 2;
        for (int chunk : {37, 256, 479, 4096}) {
            const auto output = run(scenario, chunk);
            for (int ch = 0; ch < channels; ++ch)
                for (std::size_t n = 0; n < fx::kOutputLength; ++n) {
                    const auto lane = static_cast<std::size_t>(ch);
                    chunksExact &= output[lane][n] == reference[lane][n];
                    worst = std::max(worst, std::abs(output[lane][n]-expected[static_cast<std::size_t>(scenario)][lane*fx::kOutputLength+n]));
                    scenarioWorst = std::max(scenarioWorst, std::abs(output[lane][n]-expected[static_cast<std::size_t>(scenario)][lane*fx::kOutputLength+n]));
                }
        }
        std::printf("[sidechain fixture %d] max audio error %.12g\n", scenario, scenarioWorst);
    }
    char detail[160];
    std::snprintf(detail, sizeof detail, "6 independent proto cases, max audio error %.12g", worst);
    check("sidechain", "Soft/Hard, mono-M/S, RMS mono, missing, source/mode/connection switches", worst < 1e-7, detail);
    check("sidechain", "arbitrary and oversized chunks are bit identical", chunksExact, "1/37/256/479/4096 samples, prepared maximum 97");
    const auto countReference = run(6, 1);
    bool countExact = true;
    for (int chunk : {37, 479, 4096}) countExact &= run(6, chunk) == countReference;
    check("sidechain", "mid-hop key connection count follows its final sample", countExact, "connection changes at 333/777/1051; arbitrary chunks bit exact");
    std::snprintf(detail, sizeof detail, "513-bin frame4 gain error from published float dB: %.12g", frameGainWorst);
    check("sidechain", "independent scalar Soft/Hard gain fixture", frameGainWorst < 1e-7, detail);
    check("sidechain", "prepared key processing allocates/deletes nothing", gHardAllocations == 0 && gHardDeletions == 0, "new/new[]/delete/delete[] instrumentation");
    for (int mode = 0; mode < 2; ++mode) {
        const auto withKey = run(mode, 37, false);
        SpectralNode node; node.prepare(fx::kSampleRate, 97, 2); node.setDetectionMode(mode);
        Audio legacy;
        for (int ch = 0; ch < 2; ++ch) {
            legacy[static_cast<std::size_t>(ch)].resize(fx::kOutputLength);
            for (std::size_t n = 0; n < fx::kInputLength; ++n)
                legacy[static_cast<std::size_t>(ch)][n] = static_cast<float>(fx::kMain[static_cast<std::size_t>(ch)*fx::kInputLength+n]);
        }
        float* main[]{legacy[0].data(), legacy[1].data()};
        node.process(main, 2, static_cast<int>(fx::kOutputLength));
        check("sidechain", "disabled key is bit-exact legacy output", withKey == legacy, mode == 0 ? "Soft" : "Hard");
    }
}

void verifySidechainZeroDemand() {
    SpectralNode node;
    node.prepare(48000, 256, 1);
    node.setExternalSidechainEnabled(true);
    SpectralNode::Params params;
    node.setParams(params);
    std::array<float, 256> main{}, key{};
    float* mainChannels[]{main.data()};
    const float* keyChannels[]{key.data()};
    std::array<float, SpectralNode::kNumBins> previous{}, current{}, after{};
    // Connected silence must not feed Soft's nonzero knee baseline.
    for (int hop = 0; hop < 8; ++hop) node.process(mainChannels, 1, 256, keyChannels, 1);
    node.copyReductionDb(current.data(), SpectralNode::kNumBins);
    check("sidechain", "connected silent key has exact zero Soft demand", std::all_of(current.begin(), current.end(), [](float value) { return value == 0; }), "not a floored Soft target");
    for (int hop = 0; hop < 16; ++hop) {
        for (int n = 0; n < 256; ++n) key[static_cast<std::size_t>(n)] = static_cast<float>(.3*std::sin(2*3.14159265358979323846*64*n/1024));
        previous = current;
        node.process(mainChannels, 1, 256, keyChannels, 1);
        node.copyReductionDb(current.data(), SpectralNode::kNumBins);
    }
    node.process(mainChannels, 1, 256);
    node.copyReductionDb(after.data(), SpectralNode::kNumBins);
    const double aA = std::exp(-256.0/(48000*.027));
    const double aR = std::exp(-256.0/(48000*.050));
    double error = 0.0, retained = 0.0;
    for (std::size_t bin = 0; bin < after.size(); ++bin) {
        const double s1 = (current[bin]-aA*previous[bin])/(1-aA);
        const double nextS1 = aR*s1;
        const double expected = aA*(current[bin]-nextS1)+nextS1;
        error = std::max(error, std::abs(after[bin]-expected));
        retained = std::max(retained, static_cast<double>(after[bin]));
    }
    char text[160];
    std::snprintf(text, sizeof text, "zero-target release error %.12g dB, retained %.6g dB", error, retained);
    check("sidechain", "disconnect suppresses old key-window demand without clearing release", error < 1e-5 && retained > 1.0, text);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--hard-dump") == 0) {
        const int scenario = argc > 2 ? std::atoi(argv[2]) : 0;
        const int mode = argc > 3 ? std::atoi(argv[3]) : 1;
        const auto output = runHardCase(scenario, mode, true);
        for (std::size_t i = 0; i < output[0].size(); ++i)
            std::printf("%.17g %.17g\n", static_cast<double>(output[0][i]), static_cast<double>(output[1][i]));
        return 0;
    }
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
    std::printf("\n=== 7. Weight-curve chain vs proto ===\n");
    verifyWeightChain();
    std::printf("\n=== 8. Sidebar chain vs proto ===\n");
    verifySidebar();
    std::printf("\n=== 9. M/S routing transitions ===\n");
    verifyMsModeTransitions();
    verifyActiveMsTransitions();
    std::printf("\n=== 10. Oversized host blocks ===\n");
    verifyOversizedBlocks();
    verifyNewSoftFixture();
    verifyHardFixtures();
    verifyHardRouting();
    verifyHardTransfer();
    verifyHardCapsAndSilence();
    verifyHardCalibrationAndWidth();
    verifyExternalSidechain();
    verifySidechainZeroDemand();

    std::printf("\n%s\n", gAllPass ? "ALL PASS" : "SOME FAILURES");
    return gAllPass ? 0 : 1;
}
