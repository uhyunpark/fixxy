#pragma once

#include <array>
#include <cmath>
#include <cstddef>

#include "dsp/BandResponse.h"

/**
    TPT-SVF parametric-EQ band node — Cytomic topology-preserving-transform form,
    generalized from the single peaking bell (old SvfPeakNode) to all 8 Pro-Q-class
    band types plus variable-slope Butterworth low/high-pass.

    Why this topology (the DSP "why"): a direct-form biquad stores history (z1/z2)
    valid only under the exact coefficients that produced it. Recompute coefficients
    while that history is live — which is what per-block parameter smoothing does —
    and you inject a transient the new filter was never designed to carry; at high
    Q/gain that is an audible click and can diverge. The TPT-SVF's two states
    (ic1eq, ic2eq) are physical trapezoidal-integrator states, not
    coefficient-dependent delay memory: coefficients only scale how state is read
    each sample, so they can change every block without corrupting stored energy.
    That is exactly what makes live modulation safe.

    One state-variable kernel serves all 8 types — only the (g, k, m0, m1, m2)
    read-out recipe changes per type. Low/high-pass additionally cascade
    floor(N/2) 2nd-order sections (shared cutoff g, per-section Butterworth
    pole-angle k) plus one 1st-order TPT section when the order N = slope/6 is odd.

    Ground truth: proto/multiband_eq.py. Coefficient recipes verified against
    scipy (band shapes) and scipy.signal.butter (slope cascades) to << 0.1 dB.

    Node-library contract (PROJECT.md / CLAUDE.md):
      - Uniform lifecycle: prepare() / reset() / process().
      - Parameters as DATA (BandParams), not hardcoded members — the LLM layer
        will emit these as JSON.
      - State fully encapsulated per instance, one set of integrator states PER
        CHANNEL. All state pre-allocated inline (std::array), NEVER in process().
      - Constructible/preparable OFF the audio thread; process() allocates
        nothing, takes no locks, does no I/O, and runs bounded per-sample work.

    Precision: state and coefficients are double. The recursion is
    precision-sensitive at low f0 (g = tan(pi*f0/fs) is tiny there); single
    precision would lose bits in a1 = 1/(1 + g*(g+k)).
*/
class SvfBandNode {
public:
    // Single source of truth for the type enum lives in BandResponse.h so the
    // UI's response evaluator and this kernel can never disagree on ordinal
    // values (APVTS AudioParameterChoice indices key off this ordering too).
    using Type = svfcoeffs::Type;

    /** Parameters as data. gainDb applies to bell/shelf/tilt only; slope (dB/oct)
        applies to low/high-pass only. */
    struct BandParams {
        Type type = Type::Bell;
        double f0 = 1000.0;
        double q = 0.7071;
        double gainDb = 0.0;
        double slope = 12.0;  // dB/oct; one of 6,12,18,24,30,36,48,72,96
    };

    static constexpr int kMaxChannels = 2;
    // Highest slope is 96 dB/oct -> N = 16 -> floor(N/2) = 8 second-order sections.
    static constexpr int kMaxSections = 8;

    SvfBandNode() = default;

    /** Off-thread. INTERFACE: prepare(sampleRate, maxBlockSize, numChannels).
        Capacity is fixed (kMaxChannels) for now, but numChannels is accepted and
        validated so the node interface is locked before more nodes exist. This
        filter owns no scratch, so maxBlockSize is unused. */
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        fs = sampleRate;
        activeChannels = numChannels < kMaxChannels ? numChannels : kMaxChannels;
        updateCoeffs();
        reset();
    }

    /** Zero every integrator state on every channel. */
    void reset() {
        for (auto& perCh : sectionState)
            for (auto& s : perCh)
                s = SectionState{};
        for (auto& s : onePoleState)
            s = 0.0;
    }

    /** Set parameters and recompute all section coefficients. One tan() per
        section + O(sections) work — call OFF the audio thread, or once per block
        from smoothed values. NEVER per sample. */
    void setParams(const BandParams& p) {
        params = p;
        updateCoeffs();
    }

    const BandParams& getParams() const { return params; }

    /** Hot loop. Runs the section cascade (+ optional one-pole) in place over
        numChannels of float buffers. Coefficients are already resolved; the
        per-sample kernel is pure arithmetic — no tan(), no division, no
        allocation, bounded by kMaxSections. */
    void process(float* const* channels, int numChannels, int numSamples) {
        const int nCh = numChannels < kMaxChannels ? numChannels : kMaxChannels;
        const int nSec = numSections;
        const bool onePole = hasOnePole;
        const double g1 = onePoleG;
        const bool hp1 = onePoleHighpass;

        for (int ch = 0; ch < nCh; ++ch) {
            float* x = channels[ch];
            auto& secSt = sectionState[static_cast<std::size_t>(ch)];
            double s1 = onePoleState[static_cast<std::size_t>(ch)];

            for (int i = 0; i < numSamples; ++i) {
                double v = static_cast<double>(x[i]);

                for (int sIdx = 0; sIdx < nSec; ++sIdx) {
                    const SectionCoeffs& c = sectionCoeffs[static_cast<std::size_t>(sIdx)];
                    SectionState& st = secSt[static_cast<std::size_t>(sIdx)];
                    const double v3 = v - st.ic2eq;
                    const double v1 = c.a1 * st.ic1eq + c.a2 * v3;
                    const double v2 = st.ic2eq + c.a2 * st.ic1eq + c.a3 * v3;
                    st.ic1eq = 2.0 * v1 - st.ic1eq;
                    st.ic2eq = 2.0 * v2 - st.ic2eq;
                    v = c.m0 * v + c.m1 * v1 + c.m2 * v2;
                }

                if (onePole) {
                    const double vv = (v - s1) * g1;
                    const double lp = vv + s1;
                    s1 = lp + vv;
                    v = hp1 ? (v - lp) : lp;
                }

                x[i] = static_cast<float>(v);
            }

            onePoleState[static_cast<std::size_t>(ch)] = s1;
        }
    }

private:
    static constexpr double kPi = 3.14159265358979323846;

    struct SectionCoeffs {
        double a1 = 0.0, a2 = 0.0, a3 = 0.0;
        double m0 = 0.0, m1 = 0.0, m2 = 0.0;
    };
    struct SectionState {
        double ic1eq = 0.0, ic2eq = 0.0;
    };

    /** One SVF section's digital coefficients for a given type. The (g, k, m0,
        m1, m2) analog-prototype recipe lives in BandResponse.h — the SINGLE
        place shared with the UI's response evaluator — this just turns that
        recipe into the TPT digital form. Ground truth: proto/multiband_eq.py
        svf_coeffs(). */
    SectionCoeffs computeSection(double f0, double gainDb, double q, Type type) const {
        const svfcoeffs::SectionRecipe r = svfcoeffs::computeSectionRecipe(type, f0, gainDb, q, fs);

        SectionCoeffs c;
        c.a1 = 1.0 / (1.0 + r.g * (r.g + r.k));
        c.a2 = r.g * c.a1;
        c.a3 = r.g * c.a2;
        c.m0 = r.m0; c.m1 = r.m1; c.m2 = r.m2;
        return c;
    }

    void updateCoeffs() {
        if (params.type == Type::Lowpass || params.type == Type::Highpass) {
            // Butterworth pole-angle allocation shared with BandResponse.h so
            // the UI curve's cascade matches this kernel's cascade exactly.
            const svfcoeffs::CascadePlan plan =
                svfcoeffs::planLowHighPassCascade(params.slope, params.q);
            numSections = plan.numSections;

            for (int i = 0; i < plan.numSections; ++i) {
                sectionCoeffs[static_cast<std::size_t>(i)] = computeSection(
                    params.f0, 0.0, plan.sectionQ[static_cast<std::size_t>(i)], params.type);
            }

            if (plan.hasOnePole) {
                hasOnePole = true;
                const double g = std::tan(kPi * params.f0 / fs);
                onePoleG = g / (1.0 + g);
                onePoleHighpass = (params.type == Type::Highpass);
            } else {
                hasOnePole = false;
            }
        } else {
            numSections = 1;
            sectionCoeffs[0] = computeSection(params.f0, params.gainDb, params.q, params.type);
            hasOnePole = false;
        }
    }

    double fs = 44100.0;
    int activeChannels = kMaxChannels;
    BandParams params{};

    // Coefficients — shared across channels (same filter). State is per-channel.
    std::array<SectionCoeffs, kMaxSections> sectionCoeffs{};
    int numSections = 1;
    bool hasOnePole = false;
    double onePoleG = 0.0;
    bool onePoleHighpass = false;

    std::array<std::array<SectionState, kMaxSections>, kMaxChannels> sectionState{};
    std::array<double, kMaxChannels> onePoleState{};
};
