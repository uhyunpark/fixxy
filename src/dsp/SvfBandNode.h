#pragma once

#include <array>
#include <cmath>
#include <cstddef>

#include "dsp/BandResponse.h"
#include "dsp/MatchedCoeffs.h"

/**
    Hybrid parametric-EQ band node.

    TWO kernels live here, chosen per type at coefficient-update time:

      - Bell / LowShelf / HighShelf / Tilt / Notch / Bandpass use the Ivantsov
        MATCHED design (proto/matched_kernel.py) realized in DIRECT FORM I
        (states = literal past input/output samples). This decramps the response
        near Nyquist — the digital magnitude tracks the analog prototype to
        ~0.5 dB in the musical core instead of the bilinear transform's several-dB
        squeeze.

      - Lowpass / Highpass (all slopes) KEEP the TPT-SVF Butterworth cascade
        below unchanged: a matched per-section LP cascade accumulates up to
        4.35 dB of Nyquist residual at 96 dB/oct (proto test 7 carve-out), so the
        BLT cascade — exact at Nyquist by construction — stays the better choice
        for steep cutoffs.

    WHY DIRECT FORM I for the matched types (this REPLACES the Stage-1 normal-form
    realization — the DSP "why"): the matched path now modulates its coefficients
    at a sub-block cadence (a dynamic band re-bakes the bell gain from a detector
    every K samples; and user f0/Q/gain drags already recompute per block). A
    realization whose STATE is an abstract eigenbasis (the old normal form:
    A=[[re,im],[-im,re]]) needs that state reinterpreted whenever coefficients
    change, and any diagonalizing basis is ill-conditioned near a REPEATED pole.
    A matched bell's poles use q_pole = 1/(2*Q*sqrt(G)) with G = 10^(gainDb/20);
    a deep cut drives G -> 0 so q_pole crosses critical damping (a1^2 = 4*a2) for
    ANY Q — reachable both by a dynamic range stacked on a static cut AND by a
    user dragging a bell down. proto/dynamic_eq.py test 9 measured the normal form
    overshooting the smooth reference by 2.7-10x right at that crossing (even at
    K=1, i.e. no block quantization at all); Direct Form I cleared the identical
    corners at 0.8-1.0x, all 2880 grid points finite, worst peak 1.13x. DF1's
    state is x[n-1], x[n-2], y[n-1], y[n-2] — plain past samples that stay
    meaningful under ANY coefficient swap, with no reinterpretation step to go
    ill-conditioned. (Transposed-DF2 was ruled out for a different reason: its
    z1/z2 delay-line contents ARE a coefficient-dependent mix, so swapping
    mid-flight corrupts what they represent — DF1's x/y history has no such
    dependency. The normal form stays the right choice for FREQUENCY modulation,
    a pole trajectory that need not cross critical damping — matched_kernel.py's
    swept-f0 result — but that is not this node's job.)

    DYNAMIC EQ (level-dependent band gain — proto/dynamic_eq.py, VERIFIED):
    a feed-forward detector reads this band's INPUT through a matched Bandpass at
    (f0, Q_det), a smoothstep soft-knee gain computer + compressor-style
    ballistics turn the detected level into a 0..1 "demand" w, and that w scales a
    gain OFFSET added to the band's static gain. The offset is re-baked into fresh
    matched-bell coefficients every K samples and realized through the SAME DF1
    kernel. dynEnabled=false is bit-exact to the static path (the dynamics are
    simply not entered). Only the gain-bearing matched types (Bell, LowShelf,
    HighShelf, Tilt) take dynamics; Notch/Bandpass/Low/High-pass ignore dynEnabled.

    Node-library contract (docs/architecture.md / AGENTS.md):
      - Uniform lifecycle: prepare() / reset() / process().
      - Parameters as DATA (BandParams), not hardcoded members — the LLM layer
        will emit these as JSON.
      - State fully encapsulated per instance, one set of realization states PER
        CHANNEL. All state pre-allocated inline (std::array), NEVER in process().
      - Constructible/preparable OFF the audio thread; process() allocates
        nothing, takes no locks, does no I/O, and runs bounded per-sample work.

    Precision: state and coefficients are double. The recursion is
    precision-sensitive at low f0; single precision would lose bits.
*/
class SvfBandNode {
public:
    // Single source of truth for the type enum lives in BandResponse.h so the
    // UI's response evaluator and this kernel can never disagree on ordinal
    // values (APVTS AudioParameterChoice indices key off this ordering too).
    using Type = svfcoeffs::Type;

    /** Parameters as data. gainDb applies to bell/shelf/tilt only; slope (dB/oct)
        applies to low/high-pass only. The dyn* fields drive the level-dependent
        band gain (proto/dynamic_eq.py) and matter only for the gain-bearing
        matched types when dynEnabled. */
    struct BandParams {
        Type type = Type::Bell;
        double f0 = 1000.0;
        double q = 0.7071;
        double gainDb = 0.0;
        double slope = 12.0;  // dB/oct; one of 6,12,18,24,30,36,48,72,96

        // --- Dynamic EQ (proto/dynamic_eq.py) ---
        bool dynEnabled = false;       // off => bit-exact to the static path
        double dynRangeDb = 0.0;       // signed excursion [-30,+30]; sign = boost/cut direction
        double dynThresholdDb = -24.0; // detector (band-limited) threshold [-60,0]
        double dynAttackMs = 2.0;      // [0.05,100]
        double dynReleaseMs = 80.0;    // [5,1000]

        // Field-wise equality for the setParams() dirty-check. Doubles compared
        // EXACT on purpose: while a smoother ramps, each block's value differs
        // (recompute is warranted), but once it settles the value is byte-for-byte
        // identical every block (skip is correct) -- exactly when skipping the
        // transcendental recompute pays off.
        bool operator==(const BandParams& o) const {
            return type == o.type && f0 == o.f0 && q == o.q && gainDb == o.gainDb &&
                   slope == o.slope && dynEnabled == o.dynEnabled &&
                   dynRangeDb == o.dynRangeDb && dynThresholdDb == o.dynThresholdDb &&
                   dynAttackMs == o.dynAttackMs && dynReleaseMs == o.dynReleaseMs;
        }
        bool operator!=(const BandParams& o) const { return !(*this == o); }
    };

    static constexpr int kMaxChannels = 2;
    // Highest slope is 96 dB/oct -> N = 16 -> floor(N/2) = 8 second-order sections.
    static constexpr int kMaxSections = 8;

    // Internal fixed knee width for the dynamic gain computer (proto uses W=6
    // throughout its dynamic tests). Full width: u sweeps 0->1 over [T-W/2, T+W/2].
    static constexpr double kDynKneeDb = 6.0;

    // Coefficient recompute cadence for the dynamic band, in samples. proto test
    // 8 showed K has little effect vs the detector's own ballistics dynamics (a
    // block "spike" well under 0.5 dB for K up to 512 on a de-esser-class 0.5 ms
    // attack), so we pick the largest K that keeps the worst-case coefficient lag
    // small: K=64 is ~1.5 ms at 44.1 kHz. Cheaper transcendentals (one bell
    // recompute per 64 samples) with no audible envelope stepping.
    static constexpr int kDynCoeffCadence = 64;

    SvfBandNode() = default;

    /** Stagger the K-cadence coefficient re-bake so N bands in a chain don't all
        recompute their matched biquad on the SAME sample (a per-sample cost spike
        of N transcendental designs). The owner (MultibandEq) sets a per-band phase
        offset at prepare; dsp/ stays params-as-data -- this is a plain sample
        offset in [0, kDynCoeffCadence), not a band index. Applied via reset() so
        the stagger also survives an enable-fade reset(). Default 0 keeps the node
        tests' sample-0 bake behaviour unchanged. */
    void setDynCadencePhase(int phaseSamples) {
        int ph = phaseSamples % kDynCoeffCadence;
        if (ph < 0) ph += kDynCoeffCadence;
        dynCadencePhase = ph;
    }

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

    /** Zero every realization + detector state on every channel (all kernels). */
    void reset() {
        for (auto& perCh : sectionState)
            for (auto& s : perCh)
                s = SectionState{};
        for (auto& s : onePoleState)
            s = 0.0;
        for (auto& s : df1State)
            s = Df1State{};
        // Dynamic detector.
        for (auto& s : bpState)
            s = Df1State{};
        detS1 = 0.0;
        detY = 0.0;
        // Start the re-bake counter at this band's staggered phase (0 by default,
        // set per-band by MultibandEq) so a 16-band chain spreads its coefficient
        // recomputes across the cadence window instead of firing all at once.
        dynCounter = dynCadencePhase;
        lastOffsetDb = 0.0;
    }

    /** Set parameters and recompute all section coefficients. One tan()/matched
        design per update + O(sections) work — call OFF the audio thread, or once
        per block from smoothed values. NEVER per sample. */
    void setParams(const BandParams& p) {
        // Dirty-check: skip the whole recompute (a tan()/matched design + O(sections)
        // work) when nothing changed vs the last call. The plugin calls this every
        // block from smoothed values; when the smoothers have settled, p is
        // byte-identical block after block, so this is a pure win. It also composes
        // with the BLOCKER fix: a SETTLED dynamic band skips updateCoeffs entirely,
        // so dynCoeffs is never touched off the audio thread and the K-cadence
        // re-bake inside process() owns the dynamic coefficients uncontested --
        // precisely the desired steady state. Cost when it does NOT skip: ~10
        // scalar compares per band per block, off the per-sample path.
        if (p == params)
            return;
        params = p;
        updateCoeffs();
    }

    const BandParams& getParams() const { return params; }

    /** The current band gain OFFSET (signed dB) as of the last processed sample.
        A plain member the caller reads AFTER process(); dsp/ stays atomic-free —
        the PLUGIN owns any std::atomic publish for its meter/curve. 0 when the
        band is static or its type does not take dynamics. */
    double getOffsetDb() const { return lastOffsetDb; }

    /** True when this band is actually running its dynamic path (matched
        gain-bearing type AND dynEnabled). */
    bool isDynamicActive() const { return dynActive; }

    /** Hot loop. Runs the section cascade (+ optional one-pole), or the matched
        DF1 kernel (static or dynamic), in place over numChannels of float buffers.
        Coefficients are already resolved; the per-sample kernel is pure
        arithmetic — no tan(), no division, no allocation, bounded. */
    void process(float* const* channels, int numChannels, int numSamples) {
        const int nCh = numChannels < kMaxChannels ? numChannels : kMaxChannels;

        if (usesMatched) {
            if (dynActive)
                processMatchedDynamic(channels, nCh, numSamples);
            else
                processMatchedStatic(channels, nCh, numSamples);
            return;
        }

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

    // Direct Form I state: literal past input/output samples. Coefficient-
    // independent, so per-block / per-K coefficient swaps need no reinterpretation.
    struct Df1State {
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    };

    /** One SVF section's digital coefficients for a given type. Ground truth:
        proto/multiband_eq.py svf_coeffs(). */
    SectionCoeffs computeSection(double f0, double gainDb, double q, Type type) const {
        const svfcoeffs::SectionRecipe r = svfcoeffs::computeSectionRecipe(type, f0, gainDb, q, fs);

        SectionCoeffs c;
        c.a1 = 1.0 / (1.0 + r.g * (r.g + r.k));
        c.a2 = r.g * c.a1;
        c.a3 = r.g * c.a2;
        c.m0 = r.m0; c.m1 = r.m1; c.m2 = r.m2;
        return c;
    }

    /** One matched biquad through Direct Form I, in place across nCh channels.
        y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2].
        5 mul + 4 add per sample per channel; no tan(), no division, no branch in
        the loop, no allocation. */
    void processMatchedStatic(float* const* channels, int nCh, int numSamples) {
        const double b0 = matchedCoeffs.b0, b1 = matchedCoeffs.b1, b2 = matchedCoeffs.b2;
        const double a1 = matchedCoeffs.a1, a2 = matchedCoeffs.a2;

        for (int ch = 0; ch < nCh; ++ch) {
            float* x = channels[ch];
            Df1State& s = df1State[static_cast<std::size_t>(ch)];
            double x1 = s.x1, x2 = s.x2, y1 = s.y1, y2 = s.y2;
            for (int i = 0; i < numSamples; ++i) {
                const double xi = static_cast<double>(x[i]);
                const double yi = b0 * xi + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1; x1 = xi;
                y2 = y1; y1 = yi;
                x[i] = static_cast<float>(yi);
            }
            s.x1 = x1; s.x2 = x2; s.y1 = y1; s.y2 = y2;
        }
    }

    /** Dynamic matched band: feed-forward detector -> smoothstep knee ->
        smooth_decoupled_rtt ballistics -> gain offset -> K-cadence matched-bell
        recompute -> DF1 realization. Sample-outer / channel-inner so the detector
        (channel-linked, max-abs across channels — the loudest channel governs,
        no image shift) reads the band's INPUT before the band filters it, and one
        gain is applied to every channel. All ports verbatim from the proto's
        detector_trace + run_ballistics + run_dynamic_bell; nothing re-derived. */
    void processMatchedDynamic(float* const* channels, int nCh, int numSamples) {
        // Detector bandpass coefficients (fixed for the block).
        const double bpB0 = bpCoeffs.b0, bpB1 = bpCoeffs.b1, bpB2 = bpCoeffs.b2;
        const double bpA1 = bpCoeffs.a1, bpA2 = bpCoeffs.a2;
        // One detector state (channel-linked): the BP runs on the max-abs mono
        // detector signal, so a single 2nd-order history — index 0 of bpState.
        Df1State& bp = bpState[0];
        double bx1 = bp.x1, bx2 = bp.x2, by1 = bp.y1, by2 = bp.y2;

        const double kneeLo = dynThreshold - kDynKneeDb / 2.0;  // T - W/2
        const double invW = 1.0 / kDynKneeDb;

        double s1 = detS1;   // decoupled release-pole state
        double yb = detY;    // ballistics output = w_smoothed
        int counter = dynCounter;
        double offsetDb = lastOffsetDb;

        double b0 = dynCoeffs.b0, b1 = dynCoeffs.b1, b2 = dynCoeffs.b2;
        double a1 = dynCoeffs.a1, a2 = dynCoeffs.a2;

        for (int i = 0; i < numSamples; ++i) {
            // 1) Detector sidechain: max-abs across channels (from the INPUT).
            double sc = 0.0;
            for (int ch = 0; ch < nCh; ++ch) {
                const double a = std::fabs(static_cast<double>(channels[ch][i]));
                if (a > sc) sc = a;
            }

            // 2) Matched Bandpass on the detector signal (DF1).
            const double bpOut = bpB0 * sc + bpB1 * bx1 + bpB2 * bx2 - bpA1 * by1 - bpA2 * by2;
            bx2 = bx1; bx1 = sc;
            by2 = by1; by1 = bpOut;

            // 3) Per-sample peak level in dB (no envelope smoothing before the
            //    log — the proto's instantaneous-peak convention).
            const double mag = std::fabs(bpOut);
            const double level = 20.0 * std::log10(mag > 1e-6 ? mag : 1e-6);

            // 4) Smoothstep soft-knee gain computer: u=clip((L-(T-W/2))/W,0,1),
            //    w = 3u^2 - 2u^3 (Hermite; value+slope pinned 0 at u=0, 1 at u=1
            //    so the offset ramps in/out with no click, symmetric in +/-range).
            double u = (level - kneeLo) * invW;
            u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
            const double wRaw = u * u * (3.0 - 2.0 * u);

            // 5) smooth_decoupled_rtt ballistics on w (rise=attack, fall=release;
            //    no sign flip — w already rises on increasing demand).
            if (wRaw >= s1)
                s1 = wRaw;
            else
                s1 = dynAR * (s1 - wRaw) + wRaw;
            yb = dynAA * (yb - s1) + s1;

            // 6) Signed gain offset added to the static band gain.
            offsetDb = dynRange * yb;

            // 7) K-cadence coefficient re-bake (causal: uses this sample's offset
            //    at the boundary). Cheap sample-cadence check; the transcendentals
            //    fire once per K samples, off the per-sample path.
            if (counter == 0) {
                dynCoeffs = matched::computeBiquad(params.type, fs, params.f0, params.q,
                                                   params.gainDb + offsetDb);
                b0 = dynCoeffs.b0; b1 = dynCoeffs.b1; b2 = dynCoeffs.b2;
                a1 = dynCoeffs.a1; a2 = dynCoeffs.a2;
                counter = kDynCoeffCadence;
            }
            --counter;

            // 8) Apply the band (DF1) to every channel with the current coeffs.
            for (int ch = 0; ch < nCh; ++ch) {
                Df1State& st = df1State[static_cast<std::size_t>(ch)];
                const double xi = static_cast<double>(channels[ch][i]);
                const double yi = b0 * xi + b1 * st.x1 + b2 * st.x2 - a1 * st.y1 - a2 * st.y2;
                st.x2 = st.x1; st.x1 = xi;
                st.y2 = st.y1; st.y1 = yi;
                channels[ch][i] = static_cast<float>(yi);
            }
        }

        bp.x1 = bx1; bp.x2 = bx2; bp.y1 = by1; bp.y2 = by2;
        detS1 = s1;
        detY = yb;
        dynCounter = counter;
        lastOffsetDb = offsetDb;
    }

    /** alpha = exp(-1/(tau*fs)); tau <= 1us => 0 (instant). Same tau_to_alpha the
        compressor uses (proto/compressor.py), reused for the dynamic ballistics. */
    static double tauToAlpha(double tauS, double fs_) {
        if (tauS <= 1e-6)
            return 0.0;
        return std::exp(-1.0 / (tauS * fs_));
    }

    static bool isDynamicCapableType(Type t) {
        // Only gain-bearing matched types carry a meaningful gain offset. Notch /
        // Bandpass have no gain parameter; Low/High-pass are not matched types.
        return t == Type::Bell || t == Type::LowShelf || t == Type::HighShelf ||
               t == Type::Tilt;
    }

    void updateCoeffs() {
        if (params.type == Type::Lowpass || params.type == Type::Highpass) {
            usesMatched = false;
            dynActive = false;
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
            // Matched types: Ivantsov digital biquad -> Direct Form I realization.
            // Same coefficient function the UI curve draws from (MatchedCoeffs.h),
            // so drawn and heard cannot drift apart.
            usesMatched = true;
            matchedCoeffs =
                matched::computeBiquad(params.type, fs, params.f0, params.q, params.gainDb);
            numSections = 0;
            hasOnePole = false;

            // Dynamic path resolution.
            dynActive = params.dynEnabled && isDynamicCapableType(params.type);
            if (dynActive) {
                // Detector: matched Bandpass at (f0, Q_det), Q_det clamped [0.5,4]
                // (the proto's detector Q range — keeps the sidechain BP musically
                // reasonable independent of the band's own Q). Same Ivantsov design
                // the audio band uses, so nothing new to verify.
                const double qDet = params.q < 0.5 ? 0.5 : (params.q > 4.0 ? 4.0 : params.q);
                bpCoeffs = matched::computeBiquad(Type::Bandpass, fs, params.f0, qDet, 0.0);
                dynAA = tauToAlpha(params.dynAttackMs / 1000.0, fs);
                dynAR = tauToAlpha(params.dynReleaseMs / 1000.0, fs);
                dynThreshold = params.dynThresholdDb;
                dynRange = params.dynRangeDb;
                // Re-bake the dynamic coeffs at the CURRENT engaged offset, NOT
                // the static gain. WHY (this is the BLOCKER fix): the plugin calls
                // setParams() -> updateCoeffs() EVERY block from smoothed values
                // (PluginProcessor step 2). The old line here reseeded
                // dynCoeffs = matchedCoeffs (static gain, zero offset) every block,
                // but dynCounter persists across blocks, so the process loop kept
                // running those static coeffs for up to kDynCoeffCadence-1 samples
                // until the next cadence re-bake -- an audible block-rate gain
                // square wave of |offsetDb| whenever an offset was engaged. Baking
                // at params.gainDb + lastOffsetDb instead keeps ALL THREE
                // invariants: (a) the smoothed STATIC gain still flows into the
                // dynamic path every block (params.gainDb is the per-block smoothed
                // value); (b) the dynEnabled=false path is untouched (this is
                // inside the dynActive branch), so it stays bit-exact; (c) at a
                // settled endpoint lastOffsetDb is constant and equals what the
                // cadence re-bake computes, so the settled-endpoint null still
                // holds. It also leaves dynCounter's phase intact, so the K-cadence
                // stagger below is preserved (forcing counter=0 instead would reset
                // the phase every block and defeat the stagger).
                dynCoeffs = matched::computeBiquad(params.type, fs, params.f0,
                                                   params.q, params.gainDb + lastOffsetDb);
            } else {
                lastOffsetDb = 0.0;
            }
        }
    }

    double fs = 44100.0;
    int activeChannels = kMaxChannels;
    BandParams params{};

    // TPT-SVF (Low/High-pass) path. Coefficients shared across channels; state
    // per channel.
    std::array<SectionCoeffs, kMaxSections> sectionCoeffs{};
    int numSections = 1;
    bool hasOnePole = false;
    double onePoleG = 0.0;
    bool onePoleHighpass = false;
    std::array<std::array<SectionState, kMaxSections>, kMaxChannels> sectionState{};
    std::array<double, kMaxChannels> onePoleState{};

    // Matched (Ivantsov + Direct Form I) path. Coefficients shared across
    // channels; one 4-sample DF1 history PER channel, pre-allocated.
    bool usesMatched = false;
    matched::Biquad matchedCoeffs{};       // static coefficients
    std::array<Df1State, kMaxChannels> df1State{};

    // Dynamic band path (proto/dynamic_eq.py). All pre-allocated; process()-safe.
    bool dynActive = false;
    matched::Biquad bpCoeffs{};            // detector bandpass
    matched::Biquad dynCoeffs{};           // current gain-modulated band coeffs
    std::array<Df1State, 1> bpState{};     // single channel-linked detector history
    double dynAA = 0.0, dynAR = 0.0;       // ballistics attack/release alphas
    double dynThreshold = -24.0;           // detector knee centre (dB)
    double dynRange = 0.0;                 // signed excursion (dB)
    double detS1 = 0.0;                    // decoupled release-pole state
    double detY = 0.0;                     // ballistics output (w_smoothed)
    int dynCounter = 0;                    // samples until next coeff re-bake
    int dynCadencePhase = 0;               // staggered start phase for dynCounter
    double lastOffsetDb = 0.0;             // published gain offset (read post-process)
};
