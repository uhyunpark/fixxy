// Offline verification of the Ivantsov matched-coefficient kernel (the
// Direct Form I realization inside SvfBandNode) against the VERIFIED proto
// (proto/matched_kernel.py) via generated fixtures. Plain C++, no JUCE —
// same rationale as band_response_test.cpp.
//
// Gates:
//   1. Coefficients: matched::computeBiquad reproduces the proto's (b0..a2) to
//      1e-12 relative; branch flag matches the fixture; |H| to 1e-9 dB. (The
//      branch flag is a property of the coefficients, not of the realization —
//      it survives the normal-form -> DF1 switch unchanged.)
//   2. Realization: the DF1 recurrence's impulse response DFTs back to the
//      biquad transfer function (2a, random biquads both pole regimes); and the
//      ACTUAL SvfBandNode kernel's impulse response DFTs back to the transfer
//      function (2b, proto fixtures).
//   3. All-bands-off passthrough through MultibandEq is bit-exact.
//   4. A block-smoothed f0 sweep of a matched band stays finite and bounded.

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dsp/MatchedCoeffs.h"
#include "dsp/MultibandEq.h"
#include "dsp/SvfBandNode.h"
#include "matched_kernel_fixtures.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

using Type = svfcoeffs::Type;

double relErr(double got, double ref) {
    const double denom = std::abs(ref) > 1e-300 ? std::abs(ref) : 1.0;
    return std::abs(got - ref) / denom;
}

// DFT of a real IR at an arbitrary frequency (matches band_response_test).
std::complex<double> dftAt(const std::vector<float>& ir, double fs, double f) {
    std::complex<double> acc(0.0, 0.0);
    const double w = 2.0 * kPi * f / fs;
    for (std::size_t n = 0; n < ir.size(); ++n) {
        const double ph = -w * static_cast<double>(n);
        acc += static_cast<double>(ir[n]) * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return acc;
}

// Simple deterministic PRNG so the test is reproducible across platforms.
struct Lcg {
    std::uint64_t s = 0x2545F4914F6CDD1DULL;
    float next() {  // uniform in [-1, 1)
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint32_t hi = static_cast<std::uint32_t>(s >> 32);
        return static_cast<float>(hi) / 2147483648.0f - 1.0f;
    }
    double uniform(double lo, double hi) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const double u = static_cast<double>(s >> 11) / 9007199254740992.0;  // [0,1)
        return lo + u * (hi - lo);
    }
};

}  // namespace

int main() {
    bool ok = true;

    // ---- Gate 1: coefficient + magnitude reproduction vs proto fixtures ----
    {
        // The kernel's sigma MUST equal the fixtures' sigma or nothing matches.
        if (relErr(matched::kSigma, matchedfix::kSigma) != 0.0) {
            std::printf("[FAIL] kSigma mismatch: kernel=%.17g fixture=%.17g\n",
                        matched::kSigma, matchedfix::kSigma);
            ok = false;
        }

        double worstCoef = 0.0, worstMag = 0.0;
        int branchMismatch = 0;
        for (int i = 0; i < matchedfix::kNumFixtures; ++i) {
            const auto& fx = matchedfix::kFixtures[i];
            const Type type = static_cast<Type>(fx.typeOrdinal);
            const matched::Biquad bq =
                matched::computeBiquad(type, fx.fs, fx.f0, fx.q, fx.gainDb);

            worstCoef = std::max(worstCoef, relErr(bq.b0, fx.b0));
            worstCoef = std::max(worstCoef, relErr(bq.b1, fx.b1));
            worstCoef = std::max(worstCoef, relErr(bq.b2, fx.b2));
            worstCoef = std::max(worstCoef, relErr(bq.a1, fx.a1));
            worstCoef = std::max(worstCoef, relErr(bq.a2, fx.a2));

            // Branch flag is a pure property of the denominator (a1^2 vs 4a2),
            // independent of the realization. Mirror the proto's
            // normal_form_dispatch boundary perturbation so the flag matches the
            // fixture exactly on the (measure-zero) repeated-pole boundary.
            double a2b = bq.a2;
            double disc = bq.a1 * bq.a1 - 4.0 * a2b;
            if (std::abs(disc) < 1e-9) {
                a2b = (a2b != 0.0) ? a2b * (1.0 - 1e-6) : a2b - 1e-9;
                disc = bq.a1 * bq.a1 - 4.0 * a2b;
            }
            const bool isComplex = disc < 0.0;
            if (isComplex != fx.branchIsComplex) {
                ++branchMismatch;
                std::printf("[FAIL] %s: branch %s, expected %s\n", fx.name,
                            isComplex ? "complex" : "real",
                            fx.branchIsComplex ? "complex" : "real");
            }

            for (int m = 0; m < fx.nMag; ++m) {
                const double refDb = fx.mag[static_cast<std::size_t>(m)].magDb;
                // Deep nulls (notch at f0) are dominated by the log-floor
                // convention (proto adds 1e-15, we floor at 1e-300) and by
                // catastrophic cancellation near a unit-circle zero — not a
                // coefficient difference. Not meaningful for the UI curve either.
                if (refDb < -60.0) continue;
                const double got = matched::biquadMagnitudeDb(bq, fx.fs, fx.mag[static_cast<std::size_t>(m)].f);
                worstMag = std::max(worstMag, std::abs(got - refDb));
            }
        }
        const bool p1 = worstCoef < 1e-12 && worstMag < 1e-9 && branchMismatch == 0;
        std::printf("Gate 1 coeff/mag vs proto: worst coeff relErr=%.2e (< 1e-12), "
                    "worst |mag| err=%.2e dB (< 1e-9), branch mismatches=%d  [%s]\n",
                    worstCoef, worstMag, branchMismatch, p1 ? "PASS" : "FAIL");
        ok &= p1;
    }

    // ---- Gate 2a: Direct Form I recurrence == transfer function ----
    // The Stage-1 normal-form-vs-transfer algebra check is gone with the normal
    // form (SvfBandNode now realizes matched biquads in DF1 — see its header).
    // This is the DF1 replacement: run the SAME DF1 recurrence the kernel runs on
    // random stable biquads spanning BOTH pole regimes (complex-conjugate and
    // real), take the impulse response, and confirm it DFTs back to biquadResponse
    // (the transfer function). A long IR (poles kept at r <= 0.95) makes
    // truncation negligible, so the gate is tight. Absolute (not relative) error:
    // random numerators put zeros anywhere, so |H| can pass through 0 where a
    // relative metric explodes on rounding alone.
    auto df1Ir = [](const matched::Biquad& bq, int n) {
        std::vector<float> ir(static_cast<std::size_t>(n), 0.0f);
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        for (int i = 0; i < n; ++i) {
            const double xi = (i == 0) ? 1.0 : 0.0;
            const double yi = bq.b0 * xi + bq.b1 * x1 + bq.b2 * x2 - bq.a1 * y1 - bq.a2 * y2;
            x2 = x1; x1 = xi;
            y2 = y1; y1 = yi;
            ir[static_cast<std::size_t>(i)] = static_cast<float>(yi);
        }
        return ir;
    };
    {
        double worstAbs = 0.0;
        int nComplex = 0, nReal = 0;
        constexpr int kIr = 1 << 15;  // 32768; r<=0.95 poles decay well within
        const int nGrid = 64;
        Lcg rng;
        for (int trial = 0; trial < 60; ++trial) {
            double a1, a2;
            bool complexPole;
            if (trial % 2 == 0) {
                const double r = rng.uniform(0.05, 0.95);
                const double th = rng.uniform(0.05, kPi - 0.05);
                a1 = -2.0 * r * std::cos(th);
                a2 = r * r;
                complexPole = true;
            } else {
                double l1 = rng.uniform(-0.95, 0.95);
                double l2 = rng.uniform(-0.95, 0.95);
                while (std::abs(l1 - l2) < 1e-2) l2 = rng.uniform(-0.95, 0.95);
                a1 = -(l1 + l2);
                a2 = l1 * l2;
                complexPole = false;
            }
            if (std::abs(a1 * a1 - 4.0 * a2) < 1e-6) continue;  // stay off the boundary
            matched::Biquad bq;
            bq.b0 = rng.uniform(-2.0, 2.0);
            bq.b1 = rng.uniform(-2.0, 2.0);
            bq.b2 = rng.uniform(-2.0, 2.0);
            bq.a1 = a1;
            bq.a2 = a2;
            complexPole ? ++nComplex : ++nReal;

            const std::vector<float> ir = df1Ir(bq, kIr);
            for (int k = 0; k < nGrid; ++k) {
                const double w = 1e-3 + (2.0 * kPi - 2e-3) * static_cast<double>(k) / (nGrid - 1);
                const std::complex<double> hDirect = matched::biquadResponse(bq, w);
                const std::complex<double> hDf1 = dftAt(ir, 2.0 * kPi, w);  // fs=2pi => w=f
                worstAbs = std::max(worstAbs, std::abs(hDf1 - hDirect));
            }
        }
        const bool p2a = worstAbs < 1e-4 && nComplex > 0 && nReal > 0;
        std::printf("Gate 2a DF1 recurrence vs transfer (impulse->DFT): worst abs err=%.2e "
                    "(< 1e-4), %d complex / %d real branch  [%s]\n",
                    worstAbs, nComplex, nReal, p2a ? "PASS" : "FAIL");
        ok &= p2a;
    }

    // ---- Gate 2b: ACTUAL kernel impulse response DFTs to the transfer fn ----
    {
        constexpr int kIrLen = 1 << 16;  // 65536; matched poles decay well within
        double worstDb = 0.0;
        for (int i = 0; i < matchedfix::kNumFixtures; ++i) {
            const auto& fx = matchedfix::kFixtures[i];
            SvfBandNode node;
            node.prepare(fx.fs, kIrLen, 1);
            SvfBandNode::BandParams p;
            p.type = static_cast<Type>(fx.typeOrdinal);
            p.f0 = fx.f0; p.q = fx.q; p.gainDb = fx.gainDb;
            node.setParams(p);

            std::vector<float> ir(static_cast<std::size_t>(kIrLen), 0.0f);
            ir[0] = 1.0f;
            float* ch[1] = {ir.data()};
            node.process(ch, 1, kIrLen);

            for (int m = 0; m < fx.nMag; ++m) {
                const double f = fx.mag[static_cast<std::size_t>(m)].f;
                const double mag = std::abs(dftAt(ir, fx.fs, f));
                const double db = 20.0 * std::log10(mag > 1e-300 ? mag : 1e-300);
                // Skip deep-stopband bins where DFT-truncation noise dominates.
                if (fx.mag[static_cast<std::size_t>(m)].magDb < -60.0) continue;
                worstDb = std::max(worstDb, std::abs(db - fx.mag[static_cast<std::size_t>(m)].magDb));
            }
        }
        const bool p2b = worstDb < 1e-3;
        std::printf("Gate 2b kernel impulse->DFT vs transfer: worst err=%.2e dB (< 1e-3)  [%s]\n",
                    worstDb, p2b ? "PASS" : "FAIL");
        ok &= p2b;
    }

    // ---- Gate 3: all-bands-off passthrough is bit-exact ----
    {
        constexpr int nCh = 2, nS = 512;
        MultibandEq eq;
        eq.prepare(48000.0, nS, nCh);  // no band enabled -> all skipped

        std::vector<float> a(nS), b(nS), inA(nS), inB(nS);
        Lcg rng;
        for (std::size_t i = 0; i < static_cast<std::size_t>(nS); ++i) {
            a[i] = inA[i] = rng.next();
            b[i] = inB[i] = rng.next();
        }
        float* ch[2] = {a.data(), b.data()};
        eq.process(ch, nCh, nS);

        bool bitExact = true;
        for (std::size_t i = 0; i < static_cast<std::size_t>(nS); ++i)
            if (a[i] != inA[i] || b[i] != inB[i]) { bitExact = false; break; }
        std::printf("Gate 3 all-bands-off passthrough bit-exact: %s  [%s]\n",
                    bitExact ? "yes" : "NO", bitExact ? "PASS" : "FAIL");
        ok &= bitExact;
    }

    // ---- Gate 4: block-smoothed f0 sweep stays finite and bounded ----
    {
        const double fs = 48000.0;
        constexpr int block = 64;
        const int nBlocks = 3000;  // ~4 s
        SvfBandNode node;
        node.prepare(fs, block, 1);

        Lcg rng;
        double peak = 0.0;
        bool finite = true;
        std::vector<float> buf(static_cast<std::size_t>(block));
        for (int blk = 0; blk < nBlocks; ++blk) {
            const double t = static_cast<double>(blk) / nBlocks;
            const double f0 = 200.0 * std::pow(5000.0 / 200.0, t);  // 200 Hz -> 5 kHz
            SvfBandNode::BandParams p;
            p.type = Type::Bell; p.f0 = f0; p.q = 6.0; p.gainDb = 12.0;
            node.setParams(p);  // coefficient recompute per block (as in the plugin)

            for (int i = 0; i < block; ++i) buf[static_cast<std::size_t>(i)] = 0.3f * rng.next();
            float* ch[1] = {buf.data()};
            node.process(ch, 1, block);
            for (int i = 0; i < block; ++i) {
                const float v = buf[static_cast<std::size_t>(i)];
                if (!std::isfinite(v)) finite = false;
                peak = std::max(peak, std::abs(static_cast<double>(v)));
            }
        }
        const bool p4 = finite && peak < 100.0;  // ~2x the smooth-reference peak is generous
        std::printf("Gate 4 modulation sweep: finite=%s peak=%.3f (< 100)  [%s]\n",
                    finite ? "yes" : "NO", peak, p4 ? "PASS" : "FAIL");
        ok &= p4;
    }

    std::printf(ok ? "\nALL GATES PASS\n" : "\nSOME GATES FAILED\n");
    return ok ? 0 : 1;
}
