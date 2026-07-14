#pragma once

#include <array>
#include <cmath>
#include <cstddef>

/**
    Feed-forward, log-domain dynamics compressor node — the shape of the eventual
    graph-library node: prepare() off the audio thread, process() on it, reset()
    clears all state.

    Ground truth: proto/compressor.py (every equation verified by 8 asserts).
    This is a straight port of that file's Compressor / PeakDetector /
    DualPoleDetector / OnePoleHPF classes, restricted to the shipping detector
    topology (smooth_decoupled_rtt) plus the optional program-dependent dual-pole
    release. It re-derives nothing — the DSP "why" lives in the proto's docstrings.

    SIGNAL FLOW (feed-forward, from the proto docstring):
      sidechain: input -> optional SC-HPF -> |x| (or RMS)
                 -> x_L = 20*log10(max(|x|,1e-6)) dB   (peak)
                 -> or  10*log10(max(ms,1e-12)) dB     (RMS mean-square)
      gain computer:  y_G(x_L)              (quadratic soft knee, FULL-width W)
      control signal: c = y_G(x_L) - x_L    (<= 0 dB: how much to attenuate)
      ballistics:     smooth the NEGATED control e = -c >= 0 ("GR demand"),
                      then g_sm = -y(e) <= 0 dB
      output:         input * 10^((g_sm + makeup)/20), SAME gain to all channels.

    SIGN CONVENTION (matches proto exactly): the detector runs on e = -c >= 0
    (0 = no reduction, rising = more reduction demanded). Attack fires when e is
    rising, release when e is falling. This is the Giannoulis/Massberg/Reiss
    x_G(n) convention. Do NOT flip which alpha means "attack".

    STEREO LINK: a SINGLE detector drives one gain that is applied to every
    channel. The sidechain level is the max-abs across channels (max-abs stereo
    link), so the loudest channel governs the whole bus — no image shift.

    LATENCY: v1 has NO lookahead (not proto-verified). Latency = 0 samples;
    there is nothing to report to the host.

    Node-library contract (PROJECT.md / CLAUDE.md):
      - Uniform lifecycle prepare(fs, maxBlockSize, numChannels)/reset/process.
      - Parameters as DATA (Params), not hardcoded members — the LLM layer will
        emit these as JSON.
      - All state pre-allocated inline; process() allocates nothing, takes no
        locks, does no I/O, runs bounded per-sample work.
      - JUCE-free, header-only, double precision throughout the detector math.
      - Coefficient recompute (the exp() / tan() calls) happens in setParams(),
        once per block from smoothed values — NEVER per sample.

    Precision: envelope state and coefficients are double. alpha = exp(-1/(tau*fs))
    sits extremely close to 1 for long releases; single precision would lose the
    bits that distinguish a 100 ms release from a 300 ms one.
*/
class CompressorNode {
public:
    static constexpr int kMaxChannels = 2;

    // RANGE "off" sentinel: rangeDb at or above this is treated as unlimited GR
    // (the clamp is skipped). Matches the plugin's 0..96 dB control where 96 dB
    // is displayed "Off" — 96 dB of gain reduction is already far beyond any
    // musical setting, and skipping the clamp keeps the default bit-exact.
    static constexpr double kRangeOffDb = 96.0;

    /** Parameters as data. Times in ms, levels in dB, ratio unitless.
        - kneeDb is the FULL knee width W (the proto's convention): the knee spans
          [T - W/2, T + W/2]. W <= 0.01 => hard knee.
        - programDependentRelease swaps the single-pole release for the dual-pole
          blend; releaseMs is then split into a fast/slow pair (see kFastFrac...).
        - rmsMode swaps peak detection (20*log10|x|) for RMS (10*log10 mean-square).
        - scHpfOn inserts a fixed 2-pole 100 Hz highpass in the DETECTOR path only. */
    struct Params {
        double thresholdDb = -20.0;
        double ratio = 4.0;
        double kneeDb = 6.0;          // FULL width W
        double attackMs = 10.0;
        double releaseMs = 100.0;
        double makeupDb = 0.0;
        bool autoMakeup = false;
        bool programDependentRelease = false;
        bool scHpfOn = false;
        bool rmsMode = false;
        // --- MIX / RANGE / HOLD (proto verify_mix/verify_range/verify_hold) ---
        // mix: dry/wet crossfade [0..1]. out = (1-mix)*dry + mix*wet, applied
        //   LAST. 1 = fully wet (default, pre-MIX behavior). The dry path is the
        //   untouched input — makeup is NOT applied to it (parallel-compression
        //   convention: makeup compensates the compressed path's level loss and
        //   has no meaning on a signal that was never attenuated). With no
        //   lookahead both paths are sample-aligned, so this is a plain linear
        //   blend that nulls to dry at mix=0.
        double mix = 1.0;
        // rangeDb: ceiling on APPLIED gain reduction, in dB (>= 0). g_applied =
        //   max(g_sm, -rangeDb), taken AFTER ballistics smoothing, BEFORE
        //   makeup/mix. The detector's own state (y1/s1) keeps evolving fully
        //   UNCLAMPED — the clamp only touches the number used for this sample's
        //   gain, so it is a continuous max() against a constant (a slope kink at
        //   the boundary, never a value jump/click). The proto uses range_db=inf
        //   for "off"; APVTS can't carry inf, so we use a finite sentinel:
        //   rangeDb >= kRangeOffDb (96 dB) is treated as unlimited and the clamp
        //   is skipped entirely (guarantees bit-exact regression at the default).
        double rangeDb = kRangeOffDb;
        // holdMs: envelope hold time before release engages, in ms (>= 0). Only
        //   the shipped smooth_decoupled_rtt detector honors it. 0 = off (bit-
        //   identical to the pre-HOLD code path). Converted to an integer sample
        //   count in setParams(), decremented per sample in the detector.
        double holdMs = 0.0;
    };

    /** Detector topology. Production ships SmoothDecoupledRtt (or the dual-pole
        release when programDependentRelease is set). SmoothBranchingRtt is a
        VERIFICATION-ONLY selection: it realizes exactly ONE first-order pole per
        branch, so its 63.2% time equals tau by construction — that is the
        detector the proto's ballistics assert (verify_ballistics) uses to check
        the tau<->alpha mapping itself. The shipped decoupled detector cascades
        its release pole through the attack pole, which measurably shifts the
        release 63.2% point (e.g. ~109 ms for a 100 ms label at attack=10 ms) —
        a real, proto-documented property, not a bug, but not what the mapping
        test isolates. This selection is NOT in Params and NOT wired to the
        plugin; it exists so the C++ ballistics test reproduces the proto's
        numbers exactly. */
    enum class Detector { SmoothDecoupledRtt, SmoothBranchingRtt };

    CompressorNode() = default;

    /** Verification hook only — see Detector. programDependentRelease overrides
        this to the dual-pole release regardless. */
    void setDetectorKind(Detector k) { detectorKind = k; }

    /** Off-thread. INTERFACE: prepare(sampleRate, maxBlockSize, numChannels).
        This node owns no scratch buffer, so maxBlockSize is unused. Coefficients
        are resolved from the default Params; the caller sets real values via
        setParams() once per block. */
    void prepare(double sampleRate, int /*maxBlockSize*/, int numChannels) {
        fs = sampleRate;
        activeChannels = numChannels < kMaxChannels ? numChannels : kMaxChannels;
        prepareScHpf();
        setParams(params);
        reset();
    }

    /** Clear all envelope + filter state. */
    void reset() {
        // Detector state (both topologies share these members).
        y1 = 0.0;
        s1 = 0.0;
        w = 0.0;
        ms = 0.0;
        for (auto& s : scHpfState)
            s = 0.0;
        holdCounter = 0;
        maxGrDb = 0.0;
    }

    /** Set parameters and recompute all coefficients. Contains the exp()/tan()
        calls (tau->alpha, RMS tau, dual-pole poles, SC-HPF cutoff). Call OFF the
        audio thread, or once per block from smoothed values. NEVER per sample. */
    void setParams(const Params& p) {
        params = p;

        // Gain-computer statics.
        T = p.thresholdDb;
        R = p.ratio;
        W = p.kneeDb;
        makeupDb = p.makeupDb;
        autoMakeup = p.autoMakeup;
        // Auto-makeup = |T| * (1 - 1/R): restores the level a signal sitting at
        // 0 dBFS would lose, a first-order "bring the level back" estimate.
        autoMakeupDb = std::fabs(T) * (1.0 - 1.0 / R);

        // Detector selection + ballistics coefficients.
        useDualPole = p.programDependentRelease;
        aA = tauToAlpha(p.attackMs / 1000.0, fs);

        if (useDualPole) {
            // releaseMs splits into a fast/slow pair around the labelled value.
            // These fractions are the PROTO'S DEFAULTS for the dual-pole demo
            // (fast=50ms, slow=300ms at releaseMs≈120ms), expressed as ratios so
            // any release setting keeps the same fast:slow spread. D_ref/tau_w are
            // the proto's DEFAULT constants (9 dB, 200 ms) — NOT the test-specific
            // 5 dB tuning, which was a per-test choice, not the node default.
            const double releaseS = p.releaseMs / 1000.0;
            aRfast = tauToAlpha(releaseS * kFastFrac, fs);
            aRslow = tauToAlpha(releaseS * kSlowFrac, fs);
            beta = tauToAlpha(kTauWms / 1000.0, fs);
            dRef = kDrefDb;
        } else {
            aR = tauToAlpha(p.releaseMs / 1000.0, fs);
        }

        // RMS mean-square smoothing coefficient (own tau, independent of ballistics).
        useRms = p.rmsMode;
        aRms = tauToAlpha(kRmsTauMs / 1000.0, fs);

        useScHpf = p.scHpfOn;
        // SC-HPF cutoff is fixed (kScHpfHz) for v1; its coefficient only depends
        // on fs, already resolved in prepareScHpf(). Nothing per-block to redo.

        // MIX / RANGE / HOLD.
        mix = p.mix;
        rangeDb = p.rangeDb;
        rangeActive = p.rangeDb < kRangeOffDb;  // >= sentinel => unlimited
        // Integer hold length, computed once per block (NOT per sample). round(),
        // matching the proto (int(round(...))): 5 ms @ 44.1k -> 220.5 -> 220/221
        // rather than truncating. holdCounter is NOT reset here — it must survive
        // block boundaries; only reset() clears it.
        holdSamples = static_cast<int>(std::lround(p.holdMs / 1000.0 * fs));
    }

    const Params& getParams() const { return params; }

    /** The static gain-computer transfer curve y_G(inputDb) for the current
        params, in dB — the compressed output level a steady input at inputDb
        settles to (ignoring makeup/ballistics). Exposed for the verification
        harness and a future UI curve display; pure double-precision math. */
    double staticCurveDb(double inputDb) const { return gainComputer(inputDb); }

    /** The current block's maximum gain-reduction magnitude in dB (>= 0). A plain
        member the caller reads AFTER process(); dsp/ stays atomic-free. The PLUGIN
        owns the std::atomic publish for its meter. */
    double getMaxGrDb() const { return maxGrDb; }

    /** Hot loop. One detector, one gain, applied to all channels (stereo-linked).
        Coefficients are already resolved; the per-sample kernel is pure arithmetic
        — no exp(), no log beyond the detector's log10, no allocation, bounded. */
    void process(float* const* channels, int numChannels, int numSamples) {
        const int nCh = numChannels < kMaxChannels ? numChannels : kMaxChannels;
        const double makeup = autoMakeup ? autoMakeupDb : makeupDb;

        double blockMaxGr = 0.0;

        for (int i = 0; i < numSamples; ++i) {
            // 1) Sidechain level: max-abs across channels (single detector).
            double sc = 0.0;
            for (int ch = 0; ch < nCh; ++ch) {
                const double a = std::fabs(static_cast<double>(channels[ch][i]));
                if (a > sc) sc = a;
            }

            // 2) Optional SC-HPF in the detector path only. Two cascaded one-pole
            //    TPT highpasses (~12 dB/oct). Runs on the (already max-abs'd)
            //    detector signal — a single shared filter, matching the mono
            //    detector. NOTE: proto filters the raw signal THEN rectifies;
            //    here we rectify (max-abs link) first because the detector is a
            //    single mono path. For the fixed 100 Hz HPF the difference is a
            //    detector-path nuance, not an audio-path change.
            if (useScHpf) {
                for (auto& st : scHpfState)
                    sc = onePoleHighpass(sc, st);
                sc = std::fabs(sc);  // re-rectify after the (bipolar) HPF output
            }

            // 3) Level in dB: peak 20*log10, or RMS one-pole mean-square 10*log10.
            double xL;
            if (useRms) {
                ms = aRms * ms + (1.0 - aRms) * sc * sc;
                xL = 10.0 * std::log10(ms > 1e-12 ? ms : 1e-12);
            } else {
                xL = 20.0 * std::log10(sc > 1e-6 ? sc : 1e-6);
            }

            // 4) Gain computer -> control signal c <= 0 -> GR demand e = -c >= 0.
            const double yG = gainComputer(xL);
            const double e = -(yG - xL);  // = xL - yG >= 0

            // 5) Ballistics on e, then g_sm = -y(e) <= 0 dB.
            const double yE = detect(e);
            const double gSmDb = -yE;

            // 5b) RANGE: clamp the APPLIED GR at -rangeDb (a plain max() against a
            //     constant — the detector state above stays unclamped). Skipped
            //     entirely when range is off, so the default is bit-exact.
            const double gAppliedDb = rangeActive ? std::fmax(gSmDb, -rangeDb) : gSmDb;

            // Track worst APPLIED GR magnitude this block (for the meter) —
            // matches the proto returning g_clamped as its GR envelope.
            const double grMag = -gAppliedDb;  // >= 0
            if (grMag > blockMaxGr) blockMaxGr = grMag;

            // 6) Apply the SAME gain to every channel, then MIX the dry/wet.
            //    wet = dry * 10^((g_applied + makeup)/20); out = (1-mix)*dry +
            //    mix*wet. Dry bypasses makeup (see Params::mix). At mix=1 this is
            //    0*dry + 1*wet == wet exactly (finite dry) — bit-exact default.
            const double gainLin = dbToGain(gAppliedDb + makeup);
            for (int ch = 0; ch < nCh; ++ch) {
                const double dry = static_cast<double>(channels[ch][i]);
                const double wet = dry * gainLin;
                channels[ch][i] = static_cast<float>((1.0 - mix) * dry + mix * wet);
            }
        }

        maxGrDb = blockMaxGr;
    }

private:
    // ---- Dual-pole program-dependent-release defaults (proto's, not the test's) ----
    // The proto's DualPoleDetector defaults: D_ref = 9 dB, tau_w = 200 ms. The
    // demo (verify_program_dependent) used fast=50ms, slow=300ms around a 120ms
    // "compromise"; expressed as fractions of the labelled release those are:
    static constexpr double kFastFrac = 50.0 / 120.0;   // ~0.4167
    static constexpr double kSlowFrac = 300.0 / 120.0;  // 2.5
    static constexpr double kTauWms = 200.0;
    static constexpr double kDrefDb = 9.0;
    static constexpr double kRmsTauMs = 5.0;   // proto Compressor default rms_tau_ms
    static constexpr double kScHpfHz = 100.0;  // fixed SC-HPF cutoff for v1
    static constexpr double kPi = 3.14159265358979323846;

    /** alpha = exp(-1/(tau*fs)); tau to 63.2% of a step. tau <= 1us => alpha = 0
        (instant, no smoothing). Exact port of the proto's tau_to_alpha. */
    static double tauToAlpha(double tauS, double fs_) {
        if (tauS <= 1e-6)
            return 0.0;
        return std::exp(-1.0 / (tauS * fs_));
    }

    static double dbToGain(double db) {
        return std::pow(10.0, db / 20.0);
    }

    /** Quadratic soft-knee gain computer y_G(x), FULL-width W. Exact port of the
        proto's gain_computer(), three segments:
          x < T-W/2  : y = x                                       (untouched)
          |x-T|<=W/2 : y = x + (1/R-1)*(x-T+W/2)^2 / (2W)          (knee)
          x > T+W/2  : y = T + (x-T)/R                             (compressed)
        W <= 0.01 collapses the knee to a hard corner (matches proto W>0 guard;
        the proto's tests use W=0 for hard knee). */
    double gainComputer(double x) const {
        const double twoDelta = 2.0 * (x - T);
        if (twoDelta < -W)
            return x;              // below knee
        if (twoDelta > W)
            return T + (x - T) / R;  // above knee
        // Inside the knee. Guard against W ~ 0 (hard knee): with W tiny the knee
        // region is a single point; fall back to the compressed line to avoid /0.
        if (W <= 0.01)
            return (x > T) ? (T + (x - T) / R) : x;
        const double d = x - T + W / 2.0;
        return x + (1.0 / R - 1.0) * d * d / (2.0 * W);
    }

    /** Ballistics on the GR-demand signal e >= 0. Either the shipped
        smooth_decoupled_rtt peak detector, or the program-dependent dual-pole
        release. Exact ports of PeakDetector.process_sample (smooth_decoupled_rtt
        branch) and DualPoleDetector.process_sample. */
    double detect(double x) {
        if (useDualPole) {
            // Program-dependent: blend a fast and slow release pole by a slowly
            // smoothed weight w tracking how far above threshold e currently sits.
            // CAVEAT (documented in the proto, math intentionally UNCHANGED): the
            // blend w*g_slow + (1-w)*g_fast happens in ALPHA space, not tau space.
            // Alpha values for long taus cluster near 1, so the fast pole (alpha
            // farther from 1) pulls the effective release time down more than its
            // weight suggests. Whether to re-do this as a tau-space blend is a
            // LATER tuning decision — do not "fix" it here; it would diverge from
            // the verified proto.
            const double xClamped = x / dRef;
            const double wIn = xClamped < 0.0 ? 0.0 : (xClamped > 1.0 ? 1.0 : xClamped);
            w = beta * w + (1.0 - beta) * wIn;
            double y;
            if (x >= y1) {
                y = aA * (y1 - x) + x;  // attack (single normal pole)
            } else {
                const double gFast = aRfast * (y1 - x) + x;
                const double gSlow = aRslow * (y1 - x) + x;
                y = w * gSlow + (1.0 - w) * gFast;
            }
            y1 = y;
            return y;
        }

        if (detectorKind == Detector::SmoothBranchingRtt) {
            // Verification-only single-pole-per-branch detector (proto's
            // smooth_branching_rtt): y = aA*(y1-x)+x if x>=y1 else aR*(y1-x)+x.
            const double y = (x >= y1) ? (aA * (y1 - x) + x) : (aR * (y1 - x) + x);
            y1 = y;
            return y;
        }

        // smooth_decoupled_rtt (SHIP) with HOLD. The hold=0 path is bit-identical
        // to the original s1 = max(x, aR*(s1-x)+x): that max() is provably an
        // attack/release branch selector — the release pole aR*(s1-x)+x is >= x
        // whenever x < s1 and <= x whenever x >= s1 (aR in [0,1)), so max() picks
        // x (attack) on a rise and the pole (release) on a fall. HOLD inserts a
        // frozen third state between them: on entering release, freeze s1 for
        // holdSamples before the pole engages. A fresh attack (x >= s1) at any
        // point — including mid-hold — resets the counter (retrigger). With
        // holdSamples == 0 the middle branch is never taken and this reduces to
        // exactly the attack/release pair above.
        double s1New;
        if (x >= s1) {
            s1New = x;                 // attack: instant, reload hold counter
            holdCounter = holdSamples;
        } else if (holdCounter > 0) {
            s1New = s1;                // frozen — envelope holds, no pole movement
            --holdCounter;
        } else {
            s1New = aR * (s1 - x) + x; // release pole
        }
        const double y = aA * (y1 - s1New) + s1New;
        s1 = s1New;
        y1 = y;
        return y;
    }

    /** TPT one-pole highpass sample. Same integrator-state trick as SvfBandNode:
        coefficient scaledG can be re-prepared live without a click. Port of the
        proto's OnePoleHPF.process_sample. */
    double onePoleHighpass(double x, double& state) const {
        const double v = (x - state) * scHpfG;
        const double lp = v + state;
        state = lp + v;
        return x - lp;
    }

    void prepareScHpf() {
        const double g = std::tan(kPi * kScHpfHz / fs);
        scHpfG = g / (1.0 + g);
    }

    // --- config resolved by prepare/setParams ---
    double fs = 44100.0;
    int activeChannels = kMaxChannels;
    Params params{};

    // Gain-computer statics.
    double T = -20.0, R = 4.0, W = 6.0;
    double makeupDb = 0.0, autoMakeupDb = 0.0;
    bool autoMakeup = false;

    // Ballistics coefficients.
    Detector detectorKind = Detector::SmoothDecoupledRtt;
    bool useDualPole = false;
    double aA = 0.0;                  // attack (both topologies)
    double aR = 0.0;                  // single-pole release
    double aRfast = 0.0, aRslow = 0.0, beta = 0.0, dRef = kDrefDb;  // dual-pole
    bool useRms = false;
    double aRms = 0.0;
    bool useScHpf = false;
    double scHpfG = 0.0;

    // MIX / RANGE / HOLD (resolved by setParams).
    double mix = 1.0;              // dry/wet blend [0..1]
    double rangeDb = kRangeOffDb;  // GR ceiling in dB (>= 0)
    bool rangeActive = false;      // false => clamp skipped (unlimited)
    int holdSamples = 0;           // hold length in samples (once per block)

    // --- state (cleared by reset) ---
    double y1 = 0.0;   // detector output memory
    double s1 = 0.0;   // decoupled release-pole state
    double w = 0.0;    // dual-pole blend weight
    double ms = 0.0;   // RMS mean-square accumulator
    int holdCounter = 0;  // samples remaining in the hold freeze
    std::array<double, 2> scHpfState{};  // two cascaded one-pole HPF states

    double maxGrDb = 0.0;  // published per block (read post-process)
};
