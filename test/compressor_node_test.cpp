// Offline verification of CompressorNode against proto/compressor.py's asserts.
// Ports the proto's verify_static_curve (1), verify_knee_slope (2),
// verify_ballistics (3), and verify_null_test (6), run DIRECTLY against the C++
// CompressorNode kernel. The pass thresholds and signal setups are copied from
// the proto so the numbers must match it.
//
// Plain C++ console target — deliberately kept out of the plugin build
// (CMakeLists.txt: separate add_executable, no JUCE link).

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "dsp/CompressorNode.h"

namespace {

constexpr double kFs = 44100.0;
constexpr double kPi = 3.14159265358979323846;
bool gAllPass = true;

void check(const char* tag, const char* name, bool cond, const char* detail) {
    if (!cond) gAllPass = false;
    std::printf("[%s] %s: %s -- %s\n", tag, cond ? "PASS" : "FAIL", name, detail);
}

// Settle a constant DC input at `levelDb` through the node (peak mode) and return
// the settled output level in dB. Mirrors the proto's static-curve harness:
// reset, push n_settle constant samples, read the last output.
double settledOutputDb(CompressorNode& c, double levelDb, int nSettle = 200) {
    c.reset();
    const double amp = std::pow(10.0, levelDb / 20.0);
    std::vector<float> buf(1);
    float* ch[1] = {buf.data()};
    double last = 0.0;
    for (int i = 0; i < nSettle; ++i) {
        buf[0] = static_cast<float>(amp);
        c.process(ch, 1, 1);
        last = std::fabs(static_cast<double>(buf[0]));
    }
    return 20.0 * std::log10(last > 1e-12 ? last : 1e-12);
}

// ============================================================
// 1: static curve — settled detector output matches y_G(x) closed form
// ============================================================
void verifyStaticCurve() {
    CompressorNode c;
    c.prepare(kFs, 512, 1);
    CompressorNode::Params p;
    p.thresholdDb = -20.0; p.ratio = 4.0; p.kneeDb = 6.0;
    p.attackMs = 0.0005; p.releaseMs = 0.0005;  // <= 1us -> alpha = 0 (instant)
    c.setParams(p);

    double maxErr = 0.0;
    const int nPts = 161;  // linspace(-80, 0, 161), matches proto
    for (int i = 0; i < nPts; ++i) {
        const double lv = -80.0 + 80.0 * static_cast<double>(i) / (nPts - 1);
        const double measured = settledOutputDb(c, lv);
        const double closed = c.staticCurveDb(lv);
        maxErr = std::max(maxErr, std::fabs(measured - closed));
    }
    char d[128];
    std::snprintf(d, sizeof d, "max abs error = %.6f dB over %d points", maxErr, nPts);
    check("static", "settled output matches y_G(x) closed form", maxErr < 1e-3, d);
}

// ============================================================
// 2: C1 knee — slope 1 below, 1/R above, continuous, monotonic non-increasing
// ============================================================
void verifyKneeSlope() {
    const double T = -20.0, R = 4.0, W = 6.0;
    CompressorNode c;
    c.prepare(kFs, 512, 1);
    CompressorNode::Params p;
    p.thresholdDb = T; p.ratio = R; p.kneeDb = W;
    c.setParams(p);

    constexpr std::size_t n = 4001;
    const double lo = T - 3 * W, hi = T + 3 * W;
    const double h = (hi - lo) / static_cast<double>(n - 1);
    std::vector<double> x(n), y(n), slope(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = lo + h * static_cast<double>(i);
        y[i] = c.staticCurveDb(x[i]);  // exact double-precision node math
    }
    for (std::size_t i = 1; i + 1 < n; ++i)
        slope[i] = (y[i + 1] - y[i - 1]) / (2 * h);
    slope[0] = slope[1];
    slope[n - 1] = slope[n - 2];

    const double below = 1.0, above = 1.0 / R;
    double devBelow = 0.0, devAbove = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (x[i] < T - W / 2 - 2 * h) devBelow = std::max(devBelow, std::fabs(slope[i] - below));
        if (x[i] > T + W / 2 + 2 * h) devAbove = std::max(devAbove, std::fabs(slope[i] - above));
    }
    char d[160];
    std::snprintf(d, sizeof d, "max dev = %.5f", devBelow);
    check("knee", "slope == 1 well below knee", devBelow < 1e-3, d);
    std::snprintf(d, sizeof d, "max dev = %.5f", devAbove);
    check("knee", "slope == 1/R well above knee", devAbove < 1e-3, d);

    double maxJump = 0.0, maxPosStep = 0.0;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const double ds = slope[i + 1] - slope[i];
        maxJump = std::max(maxJump, std::fabs(ds));
        maxPosStep = std::max(maxPosStep, ds);
    }
    const double jumpLimit = 5.0 * (below - above) / (W / h);
    std::snprintf(d, sizeof d, "max |d(slope)| between samples = %.6f", maxJump);
    check("knee", "slope continuous (no jump at knee boundaries)", maxJump < jumpLimit, d);
    std::snprintf(d, sizeof d, "max positive step in slope = %.6f", std::max(0.0, maxPosStep));
    check("knee", "slope monotonically non-increasing 1 -> 1/R", maxPosStep <= 1e-6, d);
}

// ============================================================
// 3: ballistics — 63.2% attack/release time (branching detector), level independence
// ============================================================

// Constant lo for 0.5s, step to hi for 0.5s, back to lo for 0.5s. Recover the
// smoothed gain-reduction (dB) per sample from output/input (input is DC amp, so
// gr_dB = 20*log10(out/amp) = g_sm). Uses the verification-only single-pole
// branching detector so the 63.2% time equals tau by construction (see proto's
// measure_step_response note).
std::vector<double> stepGr(double lowDb, double highDb, double attackMs, double releaseMs,
                           int& e1, int& e2) {
    CompressorNode c;
    c.prepare(kFs, 4096, 1);
    c.setDetectorKind(CompressorNode::Detector::SmoothBranchingRtt);
    CompressorNode::Params p;
    p.thresholdDb = -30.0; p.ratio = 4.0; p.kneeDb = 0.0;
    p.attackMs = attackMs; p.releaseMs = releaseMs;
    c.setParams(p);

    const int seg = static_cast<int>(0.5 * kFs);
    const double ampLo = std::pow(10.0, lowDb / 20.0);
    const double ampHi = std::pow(10.0, highDb / 20.0);
    std::vector<double> amps;
    amps.reserve(3 * seg);
    for (int i = 0; i < seg; ++i) amps.push_back(ampLo);
    for (int i = 0; i < seg; ++i) amps.push_back(ampHi);
    for (int i = 0; i < seg; ++i) amps.push_back(ampLo);

    std::vector<double> gr(amps.size());
    std::vector<float> buf(1);
    float* ch[1] = {buf.data()};
    for (std::size_t i = 0; i < amps.size(); ++i) {
        buf[0] = static_cast<float>(amps[i]);
        c.process(ch, 1, 1);
        const double out = std::fabs(static_cast<double>(buf[0]));
        gr[i] = 20.0 * std::log10((out / amps[i]) + 1e-300);
    }
    e1 = seg;
    e2 = 2 * seg;
    return gr;
}

// ms from the start of `seg` until it first crosses 63.2% toward its settled value
// (last sample of seg). rising=true: crossing upward; false: crossing downward.
double timeTo63(const std::vector<double>& seg, bool rising) {
    const double start = seg.front();
    const double finalV = seg.back();
    const double target = start + 0.632 * (finalV - start);
    for (std::size_t i = 0; i < seg.size(); ++i) {
        if (rising ? (seg[i] >= target) : (seg[i] <= target))
            return static_cast<double>(i) / kFs * 1000.0;
    }
    return static_cast<double>(seg.size()) / kFs * 1000.0;
}

void verifyBallistics() {
    const double attackMs = 10.0, releaseMs = 100.0;
    int e1 = 0, e2 = 0;
    std::vector<double> gr = stepGr(-40, -10, attackMs, releaseMs, e1, e2);

    // attack window [e1, e2): gr falls (more negative) -> crossing downward
    std::vector<double> attackWin(gr.begin() + e1, gr.begin() + e2);
    const double tAttack = timeTo63(attackWin, /*rising=*/false);
    char d[192];
    std::snprintf(d, sizeof d, "measured %.3f ms vs label %.1f ms", tAttack, attackMs);
    check("ballistics", "attack time == label within 5%",
          std::fabs(tAttack - attackMs) / attackMs < 0.05, d);

    // release window [e2, end): gr rises back toward 0 -> crossing upward
    std::vector<double> relWin(gr.begin() + e2, gr.end());
    const double tRelease = timeTo63(relWin, /*rising=*/true);
    std::snprintf(d, sizeof d, "measured %.3f ms vs label %.1f ms", tRelease, releaseMs);
    check("ballistics", "release time == label within 5%",
          std::fabs(tRelease - releaseMs) / releaseMs < 0.05, d);

    // level independence: attack time for 10 dB vs 30 dB overshoot (T=-30)
    int a1 = 0, a2 = 0, b1 = 0, b2 = 0;
    std::vector<double> gr10 = stepGr(-40, -30 + 10, attackMs, releaseMs, a1, a2);
    std::vector<double> gr30 = stepGr(-40, -30 + 30, attackMs, releaseMs, b1, b2);
    std::vector<double> w10(gr10.begin() + a1, gr10.begin() + a2);
    std::vector<double> w30(gr30.begin() + b1, gr30.begin() + b2);
    const double t10 = timeTo63(w10, false);
    const double t30 = timeTo63(w30, false);
    const double relDiff = std::fabs(t10 - t30) / attackMs;
    std::snprintf(d, sizeof d, "10dB t=%.3fms, 30dB t=%.3fms, rel diff=%.3f%%",
                  t10, t30, relDiff * 100.0);
    check("ballistics", "level-independent attack time (10dB vs 30dB overshoot)",
          relDiff < 0.02, d);
}

// ============================================================
// 6: null test — R=1, makeup=0, no SC filter -> output == input
// ============================================================
void verifyNull() {
    const int n = static_cast<int>(kFs * 1.0);
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(-0.8, 0.8);
    std::vector<float> in(static_cast<std::size_t>(n)), buf(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
        in[i] = static_cast<float>(dist(rng));
        buf[i] = in[i];
    }

    CompressorNode c;
    c.prepare(kFs, n, 1);
    CompressorNode::Params p;
    p.thresholdDb = -20.0; p.ratio = 1.0; p.kneeDb = 6.0;
    p.attackMs = 5.0; p.releaseMs = 100.0; p.makeupDb = 0.0; p.autoMakeup = false;
    c.setParams(p);

    float* ch[1] = {buf.data()};
    c.process(ch, 1, n);

    double err = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i)
        err = std::max(err, std::fabs(static_cast<double>(buf[i]) - static_cast<double>(in[i])));
    char d[96];
    std::snprintf(d, sizeof d, "R=1, makeup=0, no SC filter -> max|out-in| = %.3e", err);
    check("null", "R=1 identity", err < 1e-6, d);
}

// ============================================================
// Reference kernel: the PRE-MIX/RANGE/HOLD process() path, reimplemented here
// verbatim from CompressorNode.h (peak detect, smooth_decoupled_rtt, single-pole
// release, hold=0, no range clamp, makeup added, output = x * gainLin). Used to
// prove mix=1 / range-off / hold=0 is bit-exact vs the old code path.
// ============================================================
double tauToAlphaRef(double tauS, double fs) {
    if (tauS <= 1e-6) return 0.0;
    return std::exp(-1.0 / (tauS * fs));
}

double gainComputerRef(double x, double T, double R, double W) {
    const double twoDelta = 2.0 * (x - T);
    if (twoDelta < -W) return x;
    if (twoDelta > W) return T + (x - T) / R;
    if (W <= 0.01) return (x > T) ? (T + (x - T) / R) : x;
    const double d = x - T + W / 2.0;
    return x + (1.0 / R - 1.0) * d * d / (2.0 * W);
}

// Old serial-comp path on a mono buffer, returns output per sample.
std::vector<double> refProcessOld(const std::vector<float>& in, double T, double R,
                                  double W, double attackMs, double releaseMs,
                                  double makeupDb) {
    const double aA = tauToAlphaRef(attackMs / 1000.0, kFs);
    const double aR = tauToAlphaRef(releaseMs / 1000.0, kFs);
    double y1 = 0.0, s1 = 0.0;
    std::vector<double> out(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const double sc = std::fabs(static_cast<double>(in[i]));
        const double xL = 20.0 * std::log10(sc > 1e-6 ? sc : 1e-6);
        const double yG = gainComputerRef(xL, T, R, W);
        const double e = -(yG - xL);
        s1 = std::fmax(e, aR * (s1 - e) + e);
        const double y = aA * (y1 - s1) + s1;
        y1 = y;
        const double gSmDb = -y;
        const double gainLin = std::pow(10.0, (gSmDb + makeupDb) / 20.0);
        // Cast through float exactly like the node's output store, so the
        // comparison is a true bit-for-bit match (not a float-vs-double mismatch).
        out[i] = static_cast<double>(static_cast<float>(static_cast<double>(in[i]) * gainLin));
    }
    return out;
}

// Process a mono node one sample at a time, returning per-sample applied GR (dB,
// >= 0). getMaxGrDb() over a 1-sample block == that sample's applied GR.
std::vector<double> perSampleGr(CompressorNode& c, const std::vector<float>& in) {
    std::vector<double> gr(in.size());
    std::vector<float> buf(1);
    float* ch[1] = {buf.data()};
    for (std::size_t i = 0; i < in.size(); ++i) {
        buf[0] = in[i];
        c.process(ch, 1, 1);
        gr[i] = c.getMaxGrDb();
    }
    return gr;
}

// ============================================================
// 9: MIX — parallel-compression dry/wet crossfade
// ============================================================
void verifyMix() {
    const double T = -20.0, R = 4.0, W = 3.0, atk = 5.0, rel = 100.0;
    const int n = static_cast<int>(kFs * 1.0);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> dist(-0.7, 0.7);
    std::vector<float> x(static_cast<std::size_t>(n));
    for (auto& v : x) v = static_cast<float>(dist(rng));

    auto runNode = [&](double mix, double makeupDb, double R_) {
        CompressorNode c;
        c.prepare(kFs, n, 1);
        CompressorNode::Params p;
        p.thresholdDb = T; p.ratio = R_; p.kneeDb = W;
        p.attackMs = atk; p.releaseMs = rel; p.makeupDb = makeupDb; p.mix = mix;
        c.setParams(p);
        std::vector<float> buf = x;
        float* ch[1] = {buf.data()};
        c.process(ch, 1, n);
        return buf;
    };

    char d[192];

    // (i) mix=1 bit-exact vs the pre-MIX reference (x*gainLin, no dry blend).
    std::vector<float> yMix1 = runNode(1.0, 0.0, R);
    std::vector<double> ref = refProcessOld(x, T, R, W, atk, rel, 0.0);
    double errMix1 = 0.0;
    for (int i = 0; i < n; ++i)
        errMix1 = std::max(errMix1, std::fabs(static_cast<double>(yMix1[static_cast<std::size_t>(i)]) - ref[static_cast<std::size_t>(i)]));
    std::snprintf(d, sizeof d, "max abs diff = %.3e", errMix1);
    check("mix", "mix=1 bit-exact vs pre-MIX formula (x*gainLin, no dry blend)", errMix1 == 0.0, d);

    // (ii) mix=0 == dry input bit-exact, makeup (6 dB) NOT applied to the dry path.
    std::vector<float> yMix0 = runNode(0.0, 6.0, R);
    double errMix0 = 0.0;
    for (int i = 0; i < n; ++i)
        errMix0 = std::max(errMix0, std::fabs(static_cast<double>(yMix0[static_cast<std::size_t>(i)]) - static_cast<double>(x[static_cast<std::size_t>(i)])));
    std::snprintf(d, sizeof d, "max abs diff = %.3e", errMix0);
    check("mix", "mix=0 bit-exact vs dry input (makeup not applied to dry path)", errMix0 == 0.0, d);

    // (iii) mix=0.5, R=1: both paths equal (no GR, sample-aligned) -> nulls to input.
    std::vector<float> yHalf = runNode(0.5, 0.0, 1.0);
    double errHalf = 0.0;
    for (int i = 0; i < n; ++i)
        errHalf = std::max(errHalf, std::fabs(static_cast<double>(yHalf[static_cast<std::size_t>(i)]) - static_cast<double>(x[static_cast<std::size_t>(i)])));
    std::snprintf(d, sizeof d, "max abs diff = %.3e", errHalf);
    check("mix", "mix=0.5, R=1 nulls against input (sample-aligned, no lookahead)", errHalf < 1e-6, d);
}

// ============================================================
// 10: RANGE — ceiling on applied GR, detector state stays unclamped
// ============================================================
void verifyRange() {
    const double T = -40.0, R = 8.0, W = 0.0, atk = 2.0, rel = 150.0;
    const double rangeDb = 6.0;
    const double dur = 1.2;
    const int n = static_cast<int>(kFs * dur);
    std::vector<float> burst(static_cast<std::size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kFs;
        if (t >= 0.15 && t < 0.9)
            burst[static_cast<std::size_t>(i)] = static_cast<float>(0.9 * std::sin(2.0 * kPi * 300.0 * t));
    }

    auto build = [&](double range) {
        CompressorNode c;
        c.prepare(kFs, 1, 1);
        CompressorNode::Params p;
        p.thresholdDb = T; p.ratio = R; p.kneeDb = W;
        p.attackMs = atk; p.releaseMs = rel; p.rangeDb = range;
        c.setParams(p);
        return c;
    };

    CompressorNode cClamped = build(rangeDb);
    CompressorNode cUnclamped = build(CompressorNode::kRangeOffDb);  // off
    std::vector<double> grC = perSampleGr(cClamped, burst);
    std::vector<double> grU = perSampleGr(cUnclamped, burst);

    char d[224];

    double maxU = 0.0, maxC = 0.0, identErr = 0.0;
    for (int i = 0; i < n; ++i) {
        maxU = std::max(maxU, grU[static_cast<std::size_t>(i)]);
        maxC = std::max(maxC, grC[static_cast<std::size_t>(i)]);
        // Clamped applied GR must equal min(unclamped GR, range) sample-by-sample.
        // This holds ONLY IF the detector state evolves identically in both nodes
        // (i.e. the clamp never feeds back into y1/s1) — that is the "detector
        // state unclamped" property, verified directly rather than via curvature.
        const double expected = std::min(grU[static_cast<std::size_t>(i)], rangeDb);
        identErr = std::max(identErr, std::fabs(grC[static_cast<std::size_t>(i)] - expected));
    }

    std::snprintf(d, sizeof d, "max unclamped |GR| = %.3f dB", maxU);
    check("range", "unclamped GR actually exceeds 20dB (test is meaningful)", maxU > 20.0, d);

    std::snprintf(d, sizeof d, "max applied |GR| = %.9f dB (limit 6.0 dB)", maxC);
    check("range", "applied GR never exceeds range_db=6 (tol 1e-9)", maxC <= 6.0 + 1e-9, d);

    std::snprintf(d, sizeof d,
                  "max |clamped - min(unclamped,6)| = %.3e dB (detector state stays unclamped)", identErr);
    check("range", "clamp is exact max() with unclamped detector state", identErr <= 1e-9, d);
}

// ============================================================
// 11: HOLD — envelope freeze between attack and release
// ============================================================
void verifyHold() {
    // Instant attack (<= 1us -> aA = 0) so the GR envelope == s1 exactly and the
    // frozen "high" value is unambiguous. Drive the NODE (not a bare detector) with
    // a loud DC burst then silence; measure when applied GR starts to fall after
    // the drop. The e fed to the detector is a clean high/0 square (well above vs
    // well below threshold), same shape as the proto's detector-level test.
    const double atk = 0.0005, rel = 100.0;
    const double T = -40.0, R = 8.0, W = 0.0;
    const double amp = std::pow(10.0, -6.0 / 20.0);  // ~0.5, well above T=-40

    auto measureHold = [&](double holdMs, int& expectedSamples, double& highGr) {
        CompressorNode c;
        c.prepare(kFs, 1, 1);
        CompressorNode::Params p;
        p.thresholdDb = T; p.ratio = R; p.kneeDb = W;
        p.attackMs = atk; p.releaseMs = rel; p.holdMs = holdMs;
        c.setParams(p);
        const int pre = 300, burst = 200;
        const int drop = pre + burst;
        const int n = drop + static_cast<int>(0.15 * kFs);
        std::vector<float> e(static_cast<std::size_t>(n), 0.0f);
        for (int i = pre; i < drop; ++i) e[static_cast<std::size_t>(i)] = static_cast<float>(amp);
        std::vector<double> gr = perSampleGr(c, e);
        highGr = gr[static_cast<std::size_t>(drop - 1)];
        const double eps = 1e-9;
        int measured = n - drop;
        for (int i = drop; i < n; ++i) {
            if (gr[static_cast<std::size_t>(i)] < highGr - eps) { measured = i - drop; break; }
        }
        expectedSamples = static_cast<int>(std::lround(holdMs / 1000.0 * kFs));
        return measured;
    };

    char d[224];
    for (double holdMs : {5.0, 50.0}) {
        int expected = 0; double high = 0.0;
        const int measured = measureHold(holdMs, expected, high);
        std::snprintf(d, sizeof d, "measured %d samples (%.4f ms) vs expected %d samples (%.1f ms label)",
                      measured, measured / kFs * 1000.0, expected, holdMs);
        check("hold", "release delayed by hold time (within 1 sample)",
              std::abs(measured - expected) <= 1, d);
    }

    // Retrigger mid-hold: a second attack during the hold window must restart the
    // counter, so the hold after the SECOND drop is the full length again.
    {
        const double holdMs = 50.0;
        CompressorNode c;
        c.prepare(kFs, 1, 1);
        CompressorNode::Params p;
        p.thresholdDb = T; p.ratio = R; p.kneeDb = W;
        p.attackMs = atk; p.releaseMs = rel; p.holdMs = holdMs;
        c.setParams(p);
        const int pre1 = 300, dipLen = 10, burst2 = 200;
        const int idx0 = pre1, idx1 = idx0 + dipLen, idx2 = idx1 + burst2;
        const int n = idx2 + static_cast<int>(0.15 * kFs);
        std::vector<float> e(static_cast<std::size_t>(n), 0.0f);
        for (int i = 0; i < idx0; ++i) e[static_cast<std::size_t>(i)] = static_cast<float>(amp);
        for (int i = idx1; i < idx2; ++i) e[static_cast<std::size_t>(i)] = static_cast<float>(amp);
        std::vector<double> gr = perSampleGr(c, e);

        const double eps = 1e-9;
        const double high = gr[static_cast<std::size_t>(idx0 - 1)];
        double minDuringDip = high;
        for (int i = idx0; i < idx1; ++i) minDuringDip = std::min(minDuringDip, gr[static_cast<std::size_t>(i)]);
        std::snprintf(d, sizeof d, "min GR during dip = %.6f (expected == high %.6f)", minDuringDip, high);
        check("hold", "retrigger: envelope held (frozen) during the short dip inside hold window",
              minDuringDip >= high - eps, d);

        const int expected = static_cast<int>(std::lround(holdMs / 1000.0 * kFs));
        int measured2 = n - idx2;
        const double high2 = gr[static_cast<std::size_t>(idx2 - 1)];
        for (int i = idx2; i < n; ++i) {
            if (gr[static_cast<std::size_t>(i)] < high2 - eps) { measured2 = i - idx2; break; }
        }
        std::snprintf(d, sizeof d, "measured %d samples vs expected %d samples (full %.1fms hold)",
                      measured2, expected, holdMs);
        check("hold", "retrigger: hold restarts fully after second drop (not shortened)",
              std::abs(measured2 - expected) <= 1, d);
    }

    // hold=0 bit-identical to the pre-HOLD path: a random positive-ish signal
    // through the default (hold=0) node must match refProcessOld exactly. This is
    // also the combined mix=1/range-off/hold=0 regression (default params).
    {
        const int n = 2000;
        std::mt19937 rng(99);
        std::normal_distribution<double> nd(0.0, 1.0);
        std::vector<float> x(static_cast<std::size_t>(n));
        for (auto& v : x) v = static_cast<float>(std::fabs(nd(rng)) * 0.3);
        CompressorNode c;
        c.prepare(kFs, n, 1);
        CompressorNode::Params p;  // all defaults: mix=1, range off, hold=0
        p.thresholdDb = -20.0; p.ratio = 4.0; p.kneeDb = 6.0;
        p.attackMs = 5.0; p.releaseMs = 120.0;
        c.setParams(p);
        std::vector<float> buf = x;
        float* ch[1] = {buf.data()};
        c.process(ch, 1, n);
        std::vector<double> ref = refProcessOld(x, -20.0, 4.0, 6.0, 5.0, 120.0, 0.0);
        double err = 0.0;
        for (int i = 0; i < n; ++i)
            err = std::max(err, std::fabs(static_cast<double>(buf[static_cast<std::size_t>(i)]) - ref[static_cast<std::size_t>(i)]));
        std::snprintf(d, sizeof d, "max abs diff = %.3e", err);
        check("hold", "hold=0/range-off/mix=1 bit-identical to pre-change path (regression)",
              err == 0.0, d);
    }
}

}  // namespace

int main() {
    std::printf("=== 1. Static curve ===\n");
    verifyStaticCurve();
    std::printf("\n=== 2. C1 knee slope ===\n");
    verifyKneeSlope();
    std::printf("\n=== 3. Ballistics ===\n");
    verifyBallistics();
    std::printf("\n=== 6. Null test ===\n");
    verifyNull();
    std::printf("\n=== 9. MIX ===\n");
    verifyMix();
    std::printf("\n=== 10. RANGE ===\n");
    verifyRange();
    std::printf("\n=== 11. HOLD ===\n");
    verifyHold();

    std::printf("\n%s\n", gAllPass ? "ALL PASS" : "SOME FAILURES");
    return gAllPass ? 0 : 1;
}
