// Offline verification of the dynamic-EQ path in SvfBandNode (feed-forward
// detector -> smoothstep knee -> smooth_decoupled_rtt ballistics -> K-cadence
// matched-bell re-bake -> Direct Form I) against the VERIFIED proto pipeline
// (proto/dynamic_eq.py, dumped via proto/dump_dynamic_fixtures.py). Plain C++,
// no JUCE -- same rationale as the other node tests.
//
// The fixtures use the IVANTSOV design (the shipping kernel), NOT dynamic_eq.py's
// incidental Vicanek reuse -- see the dump script header for that reconciliation.
//
// Gates:
//   1. offsetDb trajectory: the ACTUAL node, driven sample-by-sample, reproduces
//      the proto's per-sample gain offset to 1e-9; settled bell coeffs to 1e-12.
//   2. below-threshold bit-exactness: a dyn band on a signal below the knee floor
//      is bit-identical to the static band (offset==0 must not perturb anything).
//   3. settled-endpoint null: once the demand saturates (w->1), the K-cadence
//      dynamic realization equals a plain static bell at gain+range (< -120 dB).
//   4. up/down symmetry: offset(+range) == -offset(-range) for the same drive.
//   5. DF1 impulse vs transfer: a settled dynamic bell's impulse response DFTs
//      back to its biquad transfer function.
//   6. worst-corner sweep: Q=18, static +12, range -24, attack 0.1 ms -- the
//      deep-cut real-pole crossing that broke the old normal form -- stays finite
//      and bounded (peak <= 1.5x the static-at-loudest reference).

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dsp/MatchedCoeffs.h"
#include "dsp/SvfBandNode.h"
#include "dynamic_eq_fixtures.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
using Type = svfcoeffs::Type;

double relErr(double got, double ref) {
    const double denom = std::abs(ref) > 1e-300 ? std::abs(ref) : 1.0;
    return std::abs(got - ref) / denom;
}

// Deterministic PRNG (same shape as matched_kernel_test).
struct Lcg {
    std::uint64_t s = 0x9E3779B97F4A7C15ULL;
    float next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint32_t hi = static_cast<std::uint32_t>(s >> 32);
        return static_cast<float>(hi) / 2147483648.0f - 1.0f;
    }
};

std::complex<double> dftAt(const std::vector<float>& ir, double w) {
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t n = 0; n < ir.size(); ++n) {
        const double ph = -w * static_cast<double>(n);
        acc += static_cast<double>(ir[n]) * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return acc;
}

SvfBandNode::BandParams fixtureParams(const dynfix::DynFixture& fx) {
    SvfBandNode::BandParams p;
    p.type = static_cast<Type>(fx.typeOrdinal);
    p.f0 = fx.f0;
    p.q = fx.q;
    p.gainDb = fx.staticGainDb;
    p.dynEnabled = true;
    p.dynRangeDb = fx.rangeDb;
    p.dynThresholdDb = fx.threshDb;
    p.dynAttackMs = fx.attackMs;
    p.dynReleaseMs = fx.releaseMs;
    return p;
}

}  // namespace

int main() {
    bool ok = true;

    // ---- Gate 1: offsetDb trajectory + settled coeffs vs proto fixtures ----
    {
        double worstOffset = 0.0, worstCoef = 0.0;
        const char* worstName = "";
        for (int i = 0; i < dynfix::kNumFixtures; ++i) {
            const auto& fx = dynfix::kFixtures[i];
            SvfBandNode node;
            node.prepare(fx.fs, 1, 1);
            node.setParams(fixtureParams(fx));

            // Drive one sample at a time and read the published offset after each.
            // Node state (detector, ballistics, K counter) persists across the
            // 1-sample process() calls, so this reproduces the per-sample trace.
            for (int n = 0; n < dynfix::kNTraj; ++n) {
                float s = static_cast<float>(fx.x[static_cast<std::size_t>(n)]);
                float* ch[1] = {&s};
                node.process(ch, 1, 1);
                const double got = node.getOffsetDb();
                const double diff = std::abs(got - fx.offsetDb[static_cast<std::size_t>(n)]);
                if (diff > worstOffset) { worstOffset = diff; worstName = fx.name; }
            }

            // Settled bell coefficients at gain+range (w=1 endpoint).
            const matched::Biquad bq = matched::computeBiquad(
                static_cast<Type>(fx.typeOrdinal), fx.fs, fx.f0, fx.q,
                fx.staticGainDb + fx.rangeDb);
            worstCoef = std::max(worstCoef, relErr(bq.b0, fx.sb0));
            worstCoef = std::max(worstCoef, relErr(bq.b1, fx.sb1));
            worstCoef = std::max(worstCoef, relErr(bq.b2, fx.sb2));
            worstCoef = std::max(worstCoef, relErr(bq.a1, fx.sa1));
            worstCoef = std::max(worstCoef, relErr(bq.a2, fx.sa2));
        }
        const bool p1 = worstOffset < 1e-9 && worstCoef < 1e-12;
        std::printf("Gate 1 trajectory+coeffs vs proto: worst offset err=%.2e dB (< 1e-9, %s), "
                    "worst coeff relErr=%.2e (< 1e-12)  [%s]\n",
                    worstOffset, worstName, worstCoef, p1 ? "PASS" : "FAIL");
        ok &= p1;
    }

    // ---- Gate 2: below-threshold bit-exactness vs static ----
    // A quiet signal keeps the detector below the knee floor (T - W/2), so the
    // offset is exactly 0 and the dynamic band must be BIT-identical to the plain
    // static band. (proto test 4.)
    {
        const double fs = 44100.0;
        constexpr int nS = 4096;
        SvfBandNode::BandParams base;
        base.type = Type::Bell; base.f0 = 2000.0; base.q = 2.0; base.gainDb = 6.0;

        SvfBandNode dynNode, statNode;
        dynNode.prepare(fs, nS, 1);
        statNode.prepare(fs, nS, 1);
        SvfBandNode::BandParams pd = base;
        pd.dynEnabled = true; pd.dynRangeDb = -18.0; pd.dynThresholdDb = -10.0;
        pd.dynAttackMs = 2.0; pd.dynReleaseMs = 80.0;  // T-W/2 = -13 dB, well above the signal
        dynNode.setParams(pd);
        statNode.setParams(base);

        std::vector<float> a(nS), b(nS);
        Lcg rng;
        for (int i = 0; i < nS; ++i) {
            const float v = 0.02f * rng.next();  // quiet: guaranteed below the knee floor
            a[static_cast<std::size_t>(i)] = v;
            b[static_cast<std::size_t>(i)] = v;
        }
        float* chA[1] = {a.data()};
        float* chB[1] = {b.data()};
        dynNode.process(chA, 1, nS);
        statNode.process(chB, 1, nS);

        bool bitExact = true;
        for (int i = 0; i < nS; ++i)
            if (a[static_cast<std::size_t>(i)] != b[static_cast<std::size_t>(i)]) { bitExact = false; break; }
        const bool maxOffsetZero = std::abs(dynNode.getOffsetDb()) == 0.0;
        std::printf("Gate 2 below-threshold bit-exact vs static: %s (final offset=%.3e)  [%s]\n",
                    bitExact ? "yes" : "NO", dynNode.getOffsetDb(),
                    (bitExact && maxOffsetZero) ? "PASS" : "FAIL");
        ok &= bitExact && maxOffsetZero;
    }

    // ---- Gate 3: settled-endpoint null (dynamic settled == static at gain+range) ----
    // Loud noise + a very low threshold pins the demand at w=1 (the smoothstep
    // saturates whenever the detector level exceeds T + W/2), so the ballistics
    // converge yb -> 1 exactly and the dynamic band re-bakes the SAME coefficients
    // as a static bell at gain+range. We find the settled tail window (offset ==
    // range to 1e-9) and null the two outputs there. (proto test 5.)
    {
        const double fs = 44100.0;
        constexpr int nS = 40000;
        const double staticGain = 0.0, range = 6.0;

        SvfBandNode dynNode, statNode;
        dynNode.prepare(fs, 1, 1);
        statNode.prepare(fs, 1, 1);
        SvfBandNode::BandParams pd;
        pd.type = Type::Bell; pd.f0 = 3000.0; pd.q = 2.0; pd.gainDb = staticGain;
        pd.dynEnabled = true; pd.dynRangeDb = range; pd.dynThresholdDb = -120.0;
        pd.dynAttackMs = 1.0; pd.dynReleaseMs = 5.0;
        dynNode.setParams(pd);
        SvfBandNode::BandParams ps;
        ps.type = Type::Bell; ps.f0 = 3000.0; ps.q = 2.0; ps.gainDb = staticGain + range;
        statNode.setParams(ps);

        Lcg rng;
        std::vector<float> yDyn(nS), yStat(nS);
        std::vector<double> offset(nS);
        for (int i = 0; i < nS; ++i) {
            const float x = 0.4f * rng.next();
            float sd = x, ss = x;
            float* chd[1] = {&sd};
            float* chs[1] = {&ss};
            dynNode.process(chd, 1, 1);
            statNode.process(chs, 1, 1);
            yDyn[static_cast<std::size_t>(i)] = sd;
            yStat[static_cast<std::size_t>(i)] = ss;
            offset[static_cast<std::size_t>(i)] = dynNode.getOffsetDb();
        }

        // Longest contiguous settled tail: offset == range to 1e-9.
        int settleStart = -1;
        for (int i = nS - 1; i >= 0; --i) {
            if (std::abs(offset[static_cast<std::size_t>(i)] - range) < 1e-9)
                settleStart = i;
            else
                break;
        }
        double nullDb = 0.0;
        bool haveWindow = settleStart >= 0 && (nS - settleStart) > 2000;
        if (haveWindow) {
            double num = 0.0, den = 0.0;
            for (int i = settleStart; i < nS; ++i) {
                const double d = static_cast<double>(yDyn[static_cast<std::size_t>(i)]) -
                                 static_cast<double>(yStat[static_cast<std::size_t>(i)]);
                const double r = static_cast<double>(yStat[static_cast<std::size_t>(i)]);
                num += d * d; den += r * r;
            }
            nullDb = 20.0 * std::log10((std::sqrt(num / static_cast<double>(nS - settleStart)) + 1e-300) /
                                       (std::sqrt(den / static_cast<double>(nS - settleStart)) + 1e-300));
        }
        const bool p3 = haveWindow && nullDb < -120.0;
        std::printf("Gate 3 settled-endpoint null: window=%d samples, null=%.2f dB (< -120)  [%s]\n",
                    haveWindow ? (nS - settleStart) : 0, nullDb, p3 ? "PASS" : "FAIL");
        ok &= p3;
    }

    // ---- Gate 4: up/down symmetry (offset(+range) == -offset(-range)) ----
    {
        const double fs = 48000.0;
        constexpr int nS = 4096;
        auto runOffsets = [&](double range) {
            SvfBandNode node;
            node.prepare(fs, 1, 1);
            SvfBandNode::BandParams p;
            p.type = Type::Bell; p.f0 = 4000.0; p.q = 3.0; p.gainDb = 0.0;
            p.dynEnabled = true; p.dynRangeDb = range; p.dynThresholdDb = -25.0;
            p.dynAttackMs = 3.0; p.dynReleaseMs = 60.0;
            node.setParams(p);
            std::vector<double> off(nS);
            Lcg rng;
            for (int i = 0; i < nS; ++i) {
                float x = 0.5f * rng.next();
                float* ch[1] = {&x};
                node.process(ch, 1, 1);
                off[static_cast<std::size_t>(i)] = node.getOffsetDb();
            }
            return off;
        };
        const auto op = runOffsets(12.0);
        const auto on = runOffsets(-12.0);
        double worst = 0.0;
        for (int i = 0; i < nS; ++i)
            worst = std::max(worst, std::abs(op[static_cast<std::size_t>(i)] +
                                             on[static_cast<std::size_t>(i)]));
        const bool p4 = worst == 0.0;
        std::printf("Gate 4 up/down symmetry: max|offset(+X)+offset(-X)|=%.2e  [%s]\n",
                    worst, p4 ? "PASS" : "FAIL");
        ok &= p4;
    }

    // ---- Gate 5: DF1 impulse vs transfer (settled dynamic bell) ----
    // Take each fixture's settled coefficients, realize them through the node's
    // static DF1 path (gainDb = gain+range, dyn off), and confirm the impulse
    // response DFTs back to the biquad transfer function.
    {
        constexpr int kIr = 1 << 15;
        double worstDb = 0.0;
        for (int i = 0; i < dynfix::kNumFixtures; ++i) {
            const auto& fx = dynfix::kFixtures[i];
            const Type type = static_cast<Type>(fx.typeOrdinal);
            const double gain = fx.staticGainDb + fx.rangeDb;
            SvfBandNode node;
            node.prepare(fx.fs, kIr, 1);
            SvfBandNode::BandParams p;
            p.type = type; p.f0 = fx.f0; p.q = fx.q; p.gainDb = gain;  // dyn off
            node.setParams(p);
            std::vector<float> ir(static_cast<std::size_t>(kIr), 0.0f);
            ir[0] = 1.0f;
            float* ch[1] = {ir.data()};
            node.process(ch, 1, kIr);

            const matched::Biquad bq = matched::computeBiquad(type, fx.fs, fx.f0, fx.q, gain);
            const double freqs[5] = {fx.f0 * 0.5, fx.f0, fx.f0 * 2.0, 500.0, 5000.0};
            for (double f : freqs) {
                if (f >= fx.fs * 0.5 * 0.98) continue;
                const double w = 2.0 * kPi * f / fx.fs;
                const double db = 20.0 * std::log10(std::abs(dftAt(ir, w)) + 1e-300);
                const double ref = matched::biquadMagnitudeDb(bq, fx.fs, f);
                if (ref < -60.0) continue;
                worstDb = std::max(worstDb, std::abs(db - ref));
            }
        }
        const bool p5 = worstDb < 1e-3;
        std::printf("Gate 5 DF1 impulse->DFT vs transfer (settled): worst err=%.2e dB (< 1e-3)  [%s]\n",
                    worstDb, p5 ? "PASS" : "FAIL");
        ok &= p5;
    }

    // ---- Gate 6: worst-corner sweep (deep-cut real-pole crossing) ----
    // Q=18, static +12, range -24, attack 0.1 ms: gain sweeps +12 -> -12 as the
    // detector engages, poles pass through critical damping (a1^2 = 4a2) -- the
    // exact corner that overshot 2.7-10x under the old normal form. DF1 must stay
    // finite and within 1.5x of the static-at-loudest reference.
    {
        const auto& fx = dynfix::kFixtures[8];  // bell_worstcorner
        const double loudestGain = std::max(fx.staticGainDb, fx.staticGainDb + fx.rangeDb);

        SvfBandNode dynNode, refNode;
        dynNode.prepare(fx.fs, 1, 1);
        refNode.prepare(fx.fs, dynfix::kNTraj, 1);
        dynNode.setParams(fixtureParams(fx));
        SvfBandNode::BandParams pr;
        pr.type = static_cast<Type>(fx.typeOrdinal); pr.f0 = fx.f0; pr.q = fx.q; pr.gainDb = loudestGain;
        refNode.setParams(pr);

        std::vector<float> yRef(dynfix::kNTraj);
        double peakDyn = 0.0;
        bool finite = true;
        for (int i = 0; i < dynfix::kNTraj; ++i)
            yRef[static_cast<std::size_t>(i)] = static_cast<float>(fx.x[static_cast<std::size_t>(i)]);
        float* chr[1] = {yRef.data()};
        refNode.process(chr, 1, dynfix::kNTraj);
        double peakRef = 1e-12;
        for (int i = 0; i < dynfix::kNTraj; ++i)
            peakRef = std::max(peakRef, std::abs(static_cast<double>(yRef[static_cast<std::size_t>(i)])));

        for (int i = 0; i < dynfix::kNTraj; ++i) {
            float s = static_cast<float>(fx.x[static_cast<std::size_t>(i)]);
            float* ch[1] = {&s};
            dynNode.process(ch, 1, 1);
            if (!std::isfinite(s)) finite = false;
            peakDyn = std::max(peakDyn, std::abs(static_cast<double>(s)));
        }
        const double ratio = peakDyn / peakRef;
        const bool p6 = finite && ratio <= 1.5;
        std::printf("Gate 6 worst-corner: finite=%s peakDyn=%.4f ratio=%.4f (<= 1.5)  [%s]\n",
                    finite ? "yes" : "NO", peakDyn, ratio, p6 ? "PASS" : "FAIL");
        ok &= p6;
    }

    // ---- Gate 7: per-block setParams seam (BLOCKER regression) ----
    // PluginProcessor calls SvfBandNode::setParams() with SMOOTHED params EVERY
    // block, then process() (PluginProcessor.cpp step 2). Once a dynamic offset
    // has settled, those smoothed params are constant across blocks (== the same
    // params). The bug: updateCoeffs() unconditionally reseeded dynCoeffs to the
    // STATIC-gain matched biquad (zero offset) every block, while dynCounter
    // persists across blocks -- so an engaged offset snapped back to static for
    // up to kDynCoeffCadence-1 samples until the next cadence re-bake. Result: a
    // block-rate gain square wave of |offsetDb|. The node's own gate tests never
    // called setParams per block, so this class of bug escaped them; this seam
    // test reproduces the exact plugin calling pattern.
    //
    // Setup maximizes visibility: static gain 0 dB (a flat, all-pass bell) vs a
    // settled -6 dB cut, so the stranded samples pass the signal UNFILTERED while
    // the correct samples cut -6 dB at f0 -- the divergence lands squarely in the
    // per-block output RMS. block=50 (coprime-ish with the 64-sample cadence) so
    // the counter phase walks across block boundaries and most blocks strand.
    {
        const double fs = 44100.0;
        const int block = 50;
        const int nBlocks = 400;
        SvfBandNode::BandParams p;
        p.type = Type::Bell; p.f0 = 3000.0; p.q = 2.0; p.gainDb = 0.0;
        p.dynEnabled = true; p.dynRangeDb = -6.0; p.dynThresholdDb = -120.0;
        p.dynAttackMs = 1.0; p.dynReleaseMs = 5.0;  // pin demand at w=1 quickly

        auto run = [&](bool reSetEachBlock) {
            SvfBandNode node;
            node.prepare(fs, block, 1);
            node.setParams(p);
            Lcg rng;
            std::vector<double> env;
            std::vector<float> buf(static_cast<std::size_t>(block));
            for (int b = 0; b < nBlocks; ++b) {
                for (int i = 0; i < block; ++i)
                    buf[static_cast<std::size_t>(i)] = 0.4f * rng.next();
                if (reSetEachBlock) node.setParams(p);  // mimic PluginProcessor
                float* ch[1] = {buf.data()};
                node.process(ch, 1, block);
                double acc = 0.0;
                for (int i = 0; i < block; ++i)
                    acc += static_cast<double>(buf[static_cast<std::size_t>(i)]) *
                           static_cast<double>(buf[static_cast<std::size_t>(i)]);
                env.push_back(std::sqrt(acc / block));
            }
            return env;
        };

        const auto ref = run(false);   // node-test pattern: setParams once
        const auto seam = run(true);   // plugin pattern: setParams every block

        // Same PRNG seed in both runs -> bit-identical input, so any per-block
        // envelope divergence in the settled tail is purely the bug. Skip the
        // first half (attack transient); by then offset is pinned at -6 dB.
        double worstRel = 0.0;
        const int start = nBlocks / 2;
        for (int b = start; b < nBlocks; ++b) {
            const double r = ref[static_cast<std::size_t>(b)];
            const double d = std::abs(seam[static_cast<std::size_t>(b)] - r);
            worstRel = std::max(worstRel, d / (r > 1e-9 ? r : 1e-9));
        }
        const bool p7 = worstRel < 1e-6;
        std::printf("Gate 7 per-block setParams seam ripple: worst rel dev=%.3e (< 1e-6)  [%s]\n",
                    worstRel, p7 ? "PASS" : "FAIL");
        ok &= p7;
    }

    std::printf(ok ? "\nALL DYNAMIC-EQ GATES PASS\n" : "\nSOME DYNAMIC-EQ GATES FAILED\n");
    return ok ? 0 : 1;
}
