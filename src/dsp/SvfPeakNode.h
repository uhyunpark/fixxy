#pragma once

#include <array>
#include <cmath>
#include <cstddef>

/**
    TPT-SVF peaking (bell) EQ node — Cytomic topology-preserving-transform form.

    Why this topology (the DSP "why", for the record):
    A direct-form biquad stores history (z1/z2) that is only valid under the
    exact coefficients that produced it. Recompute coefficients while that
    history is live — which is what parameter smoothing does every block — and
    you inject a transient the new filter was never designed to carry. At high
    Q / high gain that transient is an audible click, i.e. zipper noise baked
    into the filter itself, and it can diverge. The TPT-SVF's two states
    (ic1eq, ic2eq) are *physical trapezoidal-integrator states*, not
    coefficient-dependent delay memory. Coefficients only scale how that state
    is read each sample, so they can change every block (or every sample)
    without corrupting what is stored. That is exactly what makes it safe to
    modulate — the whole reason we chose it. Verified against the RBJ biquad in
    proto/biquad_eq.py (biquad↔SVF static match = 0.0000 dB).

    Node-library contract (see PROJECT.md / CLAUDE.md):
      - Uniform lifecycle: prepare() / reset() / process().
      - Parameters as DATA (Params struct), not hardcoded members — the LLM
        layer will emit these as JSON.
      - State fully encapsulated per instance, one SVF state pair PER CHANNEL.
      - Constructible and preparable OFF the audio thread; process() allocates
        nothing, takes no locks, does no I/O, and has a branch-free per-sample
        kernel.

    Precision: state and coefficients are double. The recursion is
    precision-sensitive at low f0 (g = tan(pi*f0/fs) is tiny there), so single
    precision would lose bits in the a1 = 1/(1 + g*(g+k)) division.
*/
class SvfPeakNode {
public:
    /** Parameters as data. These are what the LLM layer will emit as JSON. */
    struct Params {
        double f0 = 1000.0;   // centre frequency, Hz
        double Q = 1.0;       // resonance / bandwidth
        double dBgain = 0.0;  // peak gain, dB (+boost / -cut)
    };

    /** Max channels this node instance can hold state for. The plugin is
        stereo; kept small and fixed so process() never allocates. Off-thread
        prepare() could size a heap buffer instead if a node ever needs more,
        but a fixed inline array keeps the hot path trivially allocation-free. */
    static constexpr int kMaxChannels = 2;

    SvfPeakNode() = default;

    /** Off-thread. Stores sample rate and computes initial coefficients.
        maxBlockSize is part of the uniform node contract (nodes that own
        scratch buffers size them here); this filter needs no scratch, so it
        is unused. */
    void prepare(double sampleRate, int /*maxBlockSize*/) {
        fs = sampleRate;
        updateCoeffs();
        reset();
    }

    /** Zero the two integrator states on every channel. */
    void reset() {
        for (auto& s : state)
            s = State{};
    }

    /** Set parameters and recompute coefficients. Call this OFF the audio
        thread, or once per block from smoothed values on the audio thread —
        the only cost here is one tan() and one division, and crucially they
        happen OUTSIDE the per-sample loop. Never call this per sample. */
    void setParams(const Params& p) {
        params = p;
        updateCoeffs();
    }

    const Params& getParams() const { return params; }

    /**
        Hot loop. Processes numChannels of interleaved-by-pointer float buffers
        in place. Coefficients are already resolved (see setParams); the
        per-sample kernel is pure arithmetic — no tan(), no division, no
        branches, no allocation.
    */
    void process(float* const* channels, int numChannels, int numSamples) {
        const double la1 = a1, la2 = a2, la3 = a3, lm1 = m1;
        const int nCh = numChannels < kMaxChannels ? numChannels : kMaxChannels;

        for (int ch = 0; ch < nCh; ++ch) {
            float* x = channels[ch];
            State& s = state[static_cast<std::size_t>(ch)];
            double ic1eq = s.ic1eq;
            double ic2eq = s.ic2eq;

            for (int i = 0; i < numSamples; ++i) {
                const double v0 = static_cast<double>(x[i]);
                const double v3 = v0 - ic2eq;
                const double v1 = la1 * ic1eq + la2 * v3;
                const double v2 = ic2eq + la2 * ic1eq + la3 * v3;
                ic1eq = 2.0 * v1 - ic1eq;
                ic2eq = 2.0 * v2 - ic2eq;
                x[i] = static_cast<float>(v0 + lm1 * v1);  // m0=1, m2=0 for the bell
            }

            s.ic1eq = ic1eq;
            s.ic2eq = ic2eq;
        }
    }

private:
    struct State {
        double ic1eq = 0.0;
        double ic2eq = 0.0;
    };

    /** Cytomic TPT-SVF peaking-bell coefficient design. Verified ground truth
        from proto/biquad_eq.py. tan() and the division live here, never in the
        sample loop. */
    void updateCoeffs() {
        const double A = std::pow(10.0, params.dBgain / 40.0);
        const double g = std::tan(3.14159265358979323846 * params.f0 / fs);
        const double k = 1.0 / (params.Q * A);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
        m1 = k * (A * A - 1.0);  // m0 = 1, m2 = 0 baked into process()
    }

    double fs = 44100.0;
    Params params{};

    // Coefficients — shared across channels (same filter), state is not.
    double a1 = 0.0, a2 = 0.0, a3 = 0.0, m1 = 0.0;

    std::array<State, kMaxChannels> state{};
};
