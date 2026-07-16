// Offline cross-check: the UI curve evaluator (matched::bandMagnitudeDb) must
// agree with the ACTUAL SvfBandNode kernel (impulse -> direct DFT at the
// frequency of interest, same harness approach as proto/multiband_eq.py) to
// < 0.1 dB everywhere meaningful. This is the guarantee that the UI curve and
// the audio-thread kernel cannot drift apart.
//
// After the Ivantsov port both paths derive from the SAME digital coefficients
// for matched types (bell/shelf/tilt/notch/bandpass): the curve is |H(e^jw)| of
// the biquad, the kernel realizes that biquad in normal form — so they now agree
// to DFT-truncation precision, far tighter than the 0.1 dB gate. Low/High-pass
// keep the TPT-SVF analog-cascade curve/kernel pair unchanged.
//
// Plain C++ console target — deliberately kept out of the plugin build
// (CMakeLists.txt: separate add_executable, no JUCE link).

#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/BandResponse.h"
#include "dsp/MatchedCoeffs.h"
#include "dsp/SvfBandNode.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;
constexpr int kIrLen = 1 << 18;  // 262144 samples; long enough for high-Q/low-f decay.

// Direct DFT (Goertzel-equivalent) magnitude of the node's impulse response
// at an arbitrary (non-bin-aligned) frequency f. No windowing needed: for a
// stable filter the impulse response has decayed to ~0 well before kIrLen,
// so the truncated sum is an accurate estimate of the true DTFT value.
double kernelMagnitudeDb(SvfBandNode::BandParams p, double f) {
    SvfBandNode node;
    node.prepare(kFs, kIrLen, 1);
    node.setParams(p);

    std::vector<float> buf(static_cast<std::size_t>(kIrLen), 0.0f);
    buf[0] = 1.0f;
    float* channels[1] = {buf.data()};
    node.process(channels, 1, kIrLen);

    std::complex<double> acc(0.0, 0.0);
    const double w = 2.0 * kPi * f / kFs;
    for (int n = 0; n < kIrLen; ++n) {
        const double phase = -w * static_cast<double>(n);
        acc += static_cast<double>(buf[static_cast<std::size_t>(n)]) *
               std::complex<double>(std::cos(phase), std::sin(phase));
    }
    const double mag = std::abs(acc);
    return 20.0 * std::log10(mag > 1.0e-300 ? mag : 1.0e-300);
}

struct Config {
    std::string name;
    SvfBandNode::BandParams params;
};

std::vector<double> logSpacedFreqs(double lo, double hi, int n) {
    std::vector<double> out(static_cast<std::size_t>(n));
    const double logLo = std::log(lo), logHi = std::log(hi);
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(n - 1);
        out[static_cast<std::size_t>(i)] = std::exp(logLo + t * (logHi - logLo));
    }
    return out;
}

}  // namespace

int main() {
    using Type = SvfBandNode::Type;

    std::vector<Config> configs;
    {
        SvfBandNode::BandParams p;
        p.type = Type::Bell; p.f0 = 1000.0; p.q = 4.0; p.gainDb = 12.0;
        configs.push_back({"bell +12dB/1k/Q4", p});
    }
    {
        SvfBandNode::BandParams p;
        p.type = Type::Tilt; p.f0 = 1000.0; p.q = 0.7071; p.gainDb = 12.0;
        configs.push_back({"tilt +12dB/1k", p});
    }
    {
        SvfBandNode::BandParams p;
        p.type = Type::LowShelf; p.f0 = 200.0; p.q = 0.7071; p.gainDb = 6.0;
        configs.push_back({"low-shelf +6dB/200", p});
    }
    {
        SvfBandNode::BandParams p;
        p.type = Type::Highpass; p.f0 = 100.0; p.q = 2.0; p.slope = 48.0;
        configs.push_back({"HP 48dB/oct 100Hz Q2", p});
    }
    {
        SvfBandNode::BandParams p;
        p.type = Type::Lowpass; p.f0 = 8000.0; p.q = 0.7071; p.slope = 96.0;
        configs.push_back({"LP 96dB/oct 8k", p});
    }
    {
        SvfBandNode::BandParams p;
        p.type = Type::Bandpass; p.f0 = 2000.0; p.q = 3.0;
        configs.push_back({"bandpass 2k Q3", p});
    }
    {
        SvfBandNode::BandParams p;
        p.type = Type::Notch; p.f0 = 500.0; p.q = 8.0;
        configs.push_back({"notch 500 Q8", p});
    }

    const std::vector<double> freqs = logSpacedFreqs(20.0, 20000.0, 60);

    bool allPass = true;
    double worstOverall = 0.0;
    std::string worstConfig;

    for (const auto& cfg : configs) {
        double worst = 0.0;
        int nCompared = 0;
        for (double f : freqs) {
            if (f >= kFs * 0.5 * 0.98) continue;  // stay clear of Nyquist

            const double kernelDb = kernelMagnitudeDb(cfg.params, f);
            // Skip deep stopband: float/DFT-truncation noise dominates there
            // and it's not meaningful for the UI curve anyway.
            if (kernelDb < -100.0) continue;

            const double analyticDb = matched::bandMagnitudeDb(
                cfg.params.type, cfg.params.f0, cfg.params.q, cfg.params.gainDb,
                cfg.params.slope, kFs, f);

            const double err = std::abs(kernelDb - analyticDb);
            worst = std::max(worst, err);
            ++nCompared;
        }

        std::printf("%-28s max err = %7.4f dB over %d points%s\n", cfg.name.c_str(), worst,
                    nCompared, worst < 0.1 ? "  [PASS]" : "  [FAIL]");

        if (worst >= 0.1) allPass = false;
        if (worst > worstOverall) {
            worstOverall = worst;
            worstConfig = cfg.name;
        }
    }

    std::printf("\nWorst overall: %.4f dB (%s)\n", worstOverall, worstConfig.c_str());
    std::printf(allPass ? "ALL CONFIGS PASS (< 0.1 dB)\n" : "SOME CONFIGS FAILED\n");

    return allPass ? 0 : 1;
}
