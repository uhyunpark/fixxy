// Offline verification of the LpngSpectral editor's UI/publish seams — the
// analyzer/meter lesson (PROJECT.md 이음새 버그) applied to the spectral display:
//
//   1. DisplayMap (spectralui) — the THREE separate scales on one plot: frequency
//      log x round-trips (100..16k); spectrum level dB -> y in its own -90..0
//      window; reduction dB -> ribbon pixel extent (0 -> zero-height, cap -> the
//      stated cap). Mixing reduction dB onto the spectrum axis is exactly the bug
//      that blanked the EQ analyzer, so these pin the two axes apart.
//
//   2. spectralmix::blend — the mix / delta / bypass output algebra, incl. the
//      key property that DELTA of a bit-near-identity (depth 0 => wet==dry) is
//      exact silence.
//
//   3. ReductionPublisher seqlock — a written snapshot reads back identically.
//
//   4. PUBLISH ROUND-TRIP through the real node: feed a resonance (strong tone +
//      low noise) into SpectralNode, copyReductionDb(), and assert the published
//      reduction peaks at the tone's bin with a real cut, while an off-tone bin
//      is barely touched — i.e. the published array reflects what the node
//      actually computed, at the right frequency.
//
// Plain C++ console target, no JUCE — every mapped/published header is JUCE-free.

#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "dsp/SpectralNode.h"
#include "plugins/spectral/ReductionPublisher.h"
#include "plugins/spectral/SpectralMix.h"
#include "plugins/spectral/ui/SpectralDisplayMap.h"

namespace {

bool gAllPass = true;

void check(const char* tag, const char* name, bool cond, const char* detail) {
    if (!cond) gAllPass = false;
    std::printf("[%s] %s: %s -- %s\n", tag, cond ? "PASS" : "FAIL", name, detail);
}

void checkNear(const char* tag, const char* name, double got, double want, double tol) {
    char d[160];
    std::snprintf(d, sizeof d, "got %.9f, want %.9f (tol %g)", got, want, tol);
    check(tag, name, std::fabs(got - want) < tol, d);
}

// ============================================================
// 1: DisplayMap — three separate scales
// ============================================================
void verifyDisplayMap() {
    using spectralui::DisplayMap;
    DisplayMap m;
    m.setBounds(11.0f, 7.0f, 803.0f, 421.0f);  // deliberately awkward bounds

    // Frequency log x: endpoints land on the edges, midpoints round-trip.
    checkNear("map", "100 Hz -> left edge", m.freqToX(DisplayMap::kMinFreq), 11.0, 1e-3);
    checkNear("map", "16 kHz -> right edge", m.freqToX(DisplayMap::kMaxFreq), 11.0 + 803.0, 1e-3);
    double maxErr = 0.0;
    for (int i = 0; i <= 100; ++i) {
        const float logMin = std::log10(DisplayMap::kMinFreq);
        const float logMax = std::log10(DisplayMap::kMaxFreq);
        const float f = std::pow(10.0f, logMin + (logMax - logMin) * static_cast<float>(i) / 100.0f);
        const float rt = m.xToFreq(m.freqToX(f));
        maxErr = std::fmax(maxErr, std::fabs(static_cast<double>(rt - f) / f));
    }
    char d[128];
    std::snprintf(d, sizeof d, "max relative |freq->x->freq err| = %.3e over 100..16k", maxErr);
    check("map", "frequency log x round-trips", maxErr < 1e-4, d);

    // Spectrum level dB -> y in the analyzer's OWN window (not any gain axis).
    checkNear("map", "spec -90 dB -> plot bottom", m.specDbToY(DisplayMap::kSpecMinDb), 7.0 + 421.0, 1e-3);
    checkNear("map", "spec 0 dB -> plot top", m.specDbToY(DisplayMap::kSpecMaxDb), 7.0, 1e-3);
    checkNear("map", "spec -45 dB -> plot middle", m.specDbToY(-45.0f), 7.0 + 421.0 * 0.5, 1e-2);
    // Clamp: a value far outside the window pins to an edge (never overflows).
    checkNear("map", "spec +30 dB clamps to top", m.specDbToY(30.0f), 7.0, 1e-3);

    // Reduction dB -> ribbon extent: 0 -> 0, cap -> full, half -> half, clamp.
    const float band = 200.0f;
    checkNear("ribbon", "0 dB reduction -> zero height", DisplayMap::ribbonExtentPx(0.0f, band), 0.0, 1e-6);
    checkNear("ribbon", "cap reduction -> full band height",
              DisplayMap::ribbonExtentPx(DisplayMap::kMaxRibbonDb, band), band, 1e-4);
    checkNear("ribbon", "half cap -> half height",
              DisplayMap::ribbonExtentPx(DisplayMap::kMaxRibbonDb * 0.5f, band), band * 0.5, 1e-4);
    checkNear("ribbon", "beyond cap clamps to full",
              DisplayMap::ribbonExtentPx(DisplayMap::kMaxRibbonDb * 4.0f, band), band, 1e-4);

    // Reduction BASELINE geometry (soothe3-style mid-well reference line). Bounds
    // (11,7,803,421): baseline at 36% from top = 7 + 0.36*421 = 158.56; cap =
    // 0.20*421 = 84.2. Ribbon 0 dB sits ON the baseline; a full cut hangs to
    // baseline + cap. baseline+cap = 242.76 is ABOVE the plot bottom (428) and
    // above -40 dBFS on the spectrum axis — the two scales stay clear.
    const float base = m.baselineY();
    const float cap = m.ribbonCapPx();
    checkNear("baseline", "baseline at 36% of well from top", base, 7.0 + 0.36 * 421.0, 1e-3);
    checkNear("baseline", "ribbon cap = 20% of well height", cap, 0.20 * 421.0, 1e-3);
    checkNear("baseline", "0 dB reduction lands ON the baseline",
              base + DisplayMap::ribbonExtentPx(0.0f, cap), base, 1e-6);
    checkNear("baseline", "max reduction hangs to baseline + cap",
              base + DisplayMap::ribbonExtentPx(DisplayMap::kMaxRibbonDb, cap), base + cap, 1e-3);
    check("baseline", "baseline sits above -40 dBFS on the spectrum axis",
          base < m.specDbToY(-40.0f),
          "reduction band stays clear of typical spectrum territory");
}

// ============================================================
// 1b: starvation decay (display ballistics) — pure trajectory
// ============================================================
void verifyDecay() {
    using spectralui::DisplayMap;
    const float coeff = 0.92004f;  // exp(-1/(30*0.4)) — the view's kDecayCoeff

    // One step pulls a value a (1-coeff) fraction toward the floor.
    checkNear("decay", "spectrum one step toward -90 floor",
              DisplayMap::decayTowardFloor(-30.0f, -90.0f, coeff), -90.0 + coeff * 60.0, 1e-4);
    checkNear("decay", "reduction one step toward 0 floor",
              DisplayMap::decayTowardFloor(12.0f, 0.0f, coeff), coeff * 12.0, 1e-4);

    // k-step closed form: floor + coeff^k * (start - floor). Iterate 60 ticks
    // (~2 s) and confirm it tracks the analytic trajectory and approaches floor.
    float v = -20.0f;
    const float start = v, floor = -90.0f;
    for (int k = 1; k <= 60; ++k) {
        v = DisplayMap::decayTowardFloor(v, floor, coeff);
        const double want = floor + std::pow(coeff, k) * (start - floor);
        if (k == 30)
            checkNear("decay", "spectrum matches coeff^k trajectory at 1 s (30 ticks)",
                      v, want, 1e-3);
    }
    check("decay", "spectrum within 3 dB of floor after 2 s starved", v < floor + 3.0f,
          "no permanent freeze — the held frame drains to the floor");

    // Reduction ribbon drains to ~0 over the same window.
    float r = 20.0f;
    for (int k = 0; k < 60; ++k)
        r = DisplayMap::decayTowardFloor(r, 0.0f, coeff);
    // 2 s = 5 time-constants: 20 * e^-5 ~ 0.135 dB -> ribbonExtentPx ~ 0.5 px (sub-pixel).
    check("decay", "reduction ribbon drains to sub-pixel (< 0.2 dB) after 2 s starved", r < 0.2f,
          "ribbon collapses to the baseline instead of holding a stale cut");
}

// ============================================================
// 2: mix / delta / bypass blend algebra
// ============================================================
void verifyBlend() {
    using spectralmix::blend;
    using spectralmix::Mode;
    const double dry = 0.3, wet = 0.2, g = 2.0;

    checkNear("mix", "normal mix=1 -> wet*gain", blend(dry, wet, 1.0, g, Mode::Normal), wet * g, 1e-12);
    checkNear("mix", "normal mix=0 -> dry*gain", blend(dry, wet, 0.0, g, Mode::Normal), dry * g, 1e-12);
    checkNear("mix", "normal mix=0.5 -> avg*gain",
              blend(dry, wet, 0.5, g, Mode::Normal), 0.5 * (dry + wet) * g, 1e-12);

    checkNear("mix", "delta -> (wet-dry)*gain, ignores mix",
              blend(dry, wet, 0.37, g, Mode::Delta), (wet - dry) * g, 1e-12);
    checkNear("mix", "delta with wet==dry -> exact silence (bypassed processor soloes to 0)",
              blend(dry, dry, 1.0, g, Mode::Delta), 0.0, 1e-12);

    checkNear("mix", "bypass -> dry, ignores mix AND gain",
              blend(dry, wet, 0.5, g, Mode::Bypass), dry, 1e-12);
}

// ============================================================
// 3: ReductionPublisher seqlock round-trip
// ============================================================
void verifyPublisher() {
    ReductionPublisher<SpectralNode::kNumBins> pub;
    std::array<float, SpectralNode::kNumBins> src{}, dst{};
    for (int i = 0; i < SpectralNode::kNumBins; ++i)
        src[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.125f;

    pub.write(src.data(), SpectralNode::kNumBins);
    const bool ok = pub.read(dst.data(), SpectralNode::kNumBins);
    bool identical = ok;
    for (int i = 0; i < SpectralNode::kNumBins && identical; ++i)
        identical = (dst[static_cast<std::size_t>(i)] == src[static_cast<std::size_t>(i)]);
    check("pub", "seqlock write then read is identical", identical,
          ok ? "all 513 bins match" : "read failed");
}

// ============================================================
// 4: publish round-trip through the real node
// ============================================================
void verifyNodePublish() {
    constexpr double fs = 48000.0;
    constexpr int N = SpectralNode::kFftSize;
    const double f0 = 3000.0;
    const int expectedBin = static_cast<int>(std::lround(f0 * N / fs));  // ~64

    // Steady 3 kHz tone over a low broadband noise floor: a coherent resonance.
    const int len = 24000;  // 0.5 s — lets the 27 ms attack settle
    std::vector<float> x(static_cast<std::size_t>(len));
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> noise(-0.02f, 0.02f);
    for (int i = 0; i < len; ++i)
        x[static_cast<std::size_t>(i)] =
            0.5f * std::sin(2.0f * 3.14159265358979f * static_cast<float>(f0) * static_cast<float>(i) / static_cast<float>(fs))
            + noise(rng);

    SpectralNode node;
    node.prepare(fs, len, 1);
    SpectralNode::Params p;  // depth 5 / detail 5 default: full active chain
    node.setParams(p);
    float* ch[1] = {x.data()};
    node.process(ch, 1, len);

    std::array<float, SpectralNode::kNumBins> red{};
    node.copyReductionDb(red.data(), SpectralNode::kNumBins);

    int peakBin = 0;
    float peak = -1.0f;
    for (int b = 0; b < SpectralNode::kNumBins; ++b)
        if (red[static_cast<std::size_t>(b)] > peak) { peak = red[static_cast<std::size_t>(b)]; peakBin = b; }

    char d[192];
    std::snprintf(d, sizeof d, "peak reduction %.2f dB at bin %d (tone bin ~%d, %.0f Hz)",
                  static_cast<double>(peak), peakBin, expectedBin, f0);
    check("node", "published reduction peaks at the tone's bin",
          std::abs(peakBin - expectedBin) <= 8, d);
    std::snprintf(d, sizeof d, "peak reduction = %.2f dB (> 3 dB expected for a strong tone)",
                  static_cast<double>(peak));
    check("node", "published peak is a real cut", peak > 3.0f, d);

    // A bin two octaves above the tone (no resonance there) is barely touched.
    const int controlBin = expectedBin * 4;  // ~12 kHz
    const float controlRed = red[static_cast<std::size_t>(controlBin)];
    std::snprintf(d, sizeof d, "off-tone bin %d reduction = %.2f dB (<< peak %.2f dB)",
                  controlBin, static_cast<double>(controlRed), static_cast<double>(peak));
    check("node", "off-tone region is left alone", controlRed < peak * 0.5f, d);

    // depth 0 => identity => the whole published curve is zero.
    SpectralNode idle;
    idle.prepare(fs, len, 1);
    SpectralNode::Params p0; p0.depth = 0.0;
    idle.setParams(p0);
    ch[0] = x.data();
    idle.process(ch, 1, len);
    std::array<float, SpectralNode::kNumBins> red0{};
    idle.copyReductionDb(red0.data(), SpectralNode::kNumBins);
    float maxIdle = 0.0f;
    for (float v : red0) maxIdle = std::fmax(maxIdle, std::fabs(v));
    std::snprintf(d, sizeof d, "max |reduction| at depth 0 = %.3e", static_cast<double>(maxIdle));
    check("node", "depth 0 publishes an all-zero reduction curve", maxIdle == 0.0f, d);
}

}  // namespace

int main() {
    std::printf("=== 1. DisplayMap (three scales) ===\n");
    verifyDisplayMap();
    std::printf("\n=== 1b. Starvation decay (display ballistics) ===\n");
    verifyDecay();
    std::printf("\n=== 2. mix/delta/bypass blend ===\n");
    verifyBlend();
    std::printf("\n=== 3. ReductionPublisher seqlock ===\n");
    verifyPublisher();
    std::printf("\n=== 4. Publish round-trip through the node ===\n");
    verifyNodePublish();

    std::printf("\n%s\n", gAllPass ? "ALL PASS" : "SOME FAILURES");
    return gAllPass ? 0 : 1;
}
