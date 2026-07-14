#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

#include <juce_dsp/juce_dsp.h>

/**
    Falling-spectrum analyzer engine for ONE tap (pre- or post-EQ). Pure
    spectral: it depends only on juce::dsp::FFT (no GUI, no CoordMap), so it can
    be unit-tested off-line by pushing known signals and reading the dB output.

    Pipeline, per 30 Hz frame (design fixed by architecture research):
      ingest  : DC-block each incoming sample (y = x[n]-x[n-1]+0.9999*y[n-1]),
                append to a per-channel circular buffer of kFftSize (memmove ingest).
      power   : window (periodic Hann pre-scaled 2/N), real FFT order 12, and
                SUM per-channel power  |FFT(L)|^2 + |FFT(R)|^2.
      (1)     : frequency smoothing — variable-width boxcar (~1/12 octave) via a
                prefix-sum, applied TWICE. O(bins), constant per bin.
      (2)     : power -> dB  (10*log10, floored at -120 dB).
      (3)     : spectral tilt +4.5 dB/oct referenced to 1 kHz (precomputed table).
      (4)     : decay ballistics per bin — out = max(new, state + p*(new-state)):
                instant attack, exponential release. p from a 0.15 s decay at 30 fps.

    All scratch is allocated in the constructor / setSampleRate; computeFrame()
    and pushBlock() allocate nothing (they run on the message thread, but the
    node-library discipline still applies).

    NOTE on the 2/N window scaling (per spec): it is NOT compensated for the Hann
    coherent gain (0.5), so an on-bin full-scale sine reads its peak ~6 dB below
    0 dBFS. This is a fixed reference for the display; the two-tone test asserts
    against that exact value.

    NOTE on the tilt: +4.5 dB/oct is the FabFilter-style aesthetic slope. An FFT
    analyzer's per-bin power for pink noise falls -3 dB/oct, so with +4.5 tilt
    pink reads +1.5 dB/oct (not perfectly flat). White noise (flat per-bin power)
    reads +4.5 dB/oct — that is what the tilt test asserts. Set kTiltDbPerOct=3
    to make pink read flat. Flagged in the report.
*/
class AnalyzerEngine {
public:
    static constexpr int kFftOrder = 12;
    static constexpr int kFftSize = 1 << kFftOrder;    // 4096
    static constexpr int kNumBins = kFftSize / 2 + 1;  // 2049
    static constexpr int kMaxChannels = 2;

    static constexpr double kDisplayFps = 30.0;
    static constexpr double kDecaySeconds = 0.15;
    static constexpr float kFloorDb = -120.0f;
    static constexpr float kInitStateDb = -240.0f;
    static constexpr float kTiltDbPerOct = 4.5f;
    static constexpr float kTiltRefHz = 1000.0f;
    static constexpr float kDcPole = 0.9999f;

    AnalyzerEngine() : fft(kFftOrder) {
        constexpr double kPi = 3.14159265358979323846;
        window.resize(static_cast<std::size_t>(kFftSize));
        for (int n = 0; n < kFftSize; ++n) {
            // Periodic (DFT-even) Hann, pre-scaled by 2/N.
            const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * n / kFftSize);
            window[static_cast<std::size_t>(n)] = static_cast<float>(w * (2.0 / kFftSize));
        }
        for (auto& b : circ)
            b.assign(static_cast<std::size_t>(kFftSize), 0.0f);
        fftBuf.assign(static_cast<std::size_t>(2 * kFftSize), 0.0f);
        power.assign(static_cast<std::size_t>(kNumBins), 0.0f);
        scratch.assign(static_cast<std::size_t>(kNumBins), 0.0f);
        prefix.assign(static_cast<std::size_t>(kNumBins) + 1, 0.0);
        binDbBuf.assign(static_cast<std::size_t>(kNumBins), kFloorDb);
        rawDbBuf.assign(static_cast<std::size_t>(kNumBins), kFloorDb);
        state.assign(static_cast<std::size_t>(kNumBins), kInitStateDb);
        halfWidth.assign(static_cast<std::size_t>(kNumBins), 0);
        tiltDb.assign(static_cast<std::size_t>(kNumBins), 0.0f);

        // out = state + p*(new-state); slow release wants p small. Retention per
        // frame (1-p) = exp(-1/nFrames) with nFrames = fps*decay = 4.5.
        const double nFrames = kDisplayFps * kDecaySeconds;
        releaseP = static_cast<float>(1.0 - std::exp(-1.0 / nFrames));

        rebuildTables(48000.0);
    }

    /** Rebuild the fs-dependent tilt table and per-bin smoothing widths. */
    void setSampleRate(double fs) {
        if (fs <= 0.0)
            fs = 48000.0;
        if (fs == sampleRate)
            return;
        rebuildTables(fs);
        for (auto& s : state)
            s = kInitStateDb;  // a rate change invalidates held peaks
    }

    void reset() {
        for (auto& b : circ)
            std::fill(b.begin(), b.end(), 0.0f);
        for (auto& s : state)
            s = kInitStateDb;
        dcX = {};
        dcY = {};
        activeChannels = 0;
    }

    /** Append raw samples per channel (DC-blocked) into the circular buffers. */
    void pushBlock(const float* const* channels, int numChannels, int numSamples) {
        const int nCh = juce::jlimit(1, kMaxChannels, numChannels);
        activeChannels = std::max(activeChannels, nCh);
        for (int ch = 0; ch < nCh; ++ch)
            ingestChannel(ch, channels[ch], numSamples);
    }

    /** Compute one frame from the current circular buffers. */
    void computeFrame() {
        std::fill(power.begin(), power.end(), 0.0f);
        const int nCh = std::max(1, activeChannels);
        for (int ch = 0; ch < nCh; ++ch)
            accumulateChannelPower(ch);

        for (int k = 0; k < kNumBins; ++k)
            rawDbBuf[static_cast<std::size_t>(k)] = powerToDb(power[static_cast<std::size_t>(k)]);

        // (1) frequency smoothing, applied twice.
        boxcarSmoothPower();
        boxcarSmoothPower();

        // (2) power->dB, (3) tilt, (4) decay ballistics.
        for (int k = 0; k < kNumBins; ++k) {
            const auto k_ = static_cast<std::size_t>(k);
            const float db = powerToDb(power[k_]) + tiltDb[k_];
            const float released = state[k_] + releaseP * (db - state[k_]);
            const float out = std::max(db, released);  // instant attack, slow release
            state[k_] = out;
            binDbBuf[k_] = out;
        }
    }

    const std::vector<float>& binDb() const { return binDbBuf; }  // full pipeline output
    const std::vector<float>& rawDb() const { return rawDbBuf; }  // pre-smoothing, pre-tilt
    double getSampleRate() const { return sampleRate; }
    static double binFrequency(int k, double fs) {
        return static_cast<double>(k) * fs / static_cast<double>(kFftSize);
    }

private:
    void rebuildTables(double fs) {
        sampleRate = fs;
        const double binHz = fs / static_cast<double>(kFftSize);
        for (int k = 0; k < kNumBins; ++k) {
            const auto k_ = static_cast<std::size_t>(k);
            const double f = static_cast<double>(k) * binHz;
            tiltDb[k_] = (f > 0.0)
                             ? kTiltDbPerOct * static_cast<float>(std::log2(f / kTiltRefHz))
                             : 0.0f;
            // ~1/12-octave total -> 1/24 octave half-width each side.
            halfWidth[k_] =
                static_cast<int>(std::lround(static_cast<double>(k) * (std::pow(2.0, 1.0 / 24.0) - 1.0)));
        }
    }

    void ingestChannel(int ch, const float* src, int numSamples) {
        if (numSamples <= 0)
            return;
        auto& buf = circ[static_cast<std::size_t>(ch)];
        const int keep = std::min(numSamples, kFftSize);
        if (numSamples < kFftSize)
            std::memmove(buf.data(), buf.data() + numSamples,
                         sizeof(float) * static_cast<std::size_t>(kFftSize - numSamples));

        // DC-block advances over ALL samples (recursive state); we store the last `keep`.
        float x1 = dcX[static_cast<std::size_t>(ch)];
        float y1 = dcY[static_cast<std::size_t>(ch)];
        const int skip = numSamples - keep;
        int di = kFftSize - keep;
        for (int n = 0; n < numSamples; ++n) {
            const float x = src[n];
            const float y = x - x1 + kDcPole * y1;
            x1 = x;
            y1 = y;
            if (n >= skip)
                buf[static_cast<std::size_t>(di++)] = y;
        }
        dcX[static_cast<std::size_t>(ch)] = x1;
        dcY[static_cast<std::size_t>(ch)] = y1;
    }

    void accumulateChannelPower(int ch) {
        auto& buf = circ[static_cast<std::size_t>(ch)];
        for (int n = 0; n < kFftSize; ++n)
            fftBuf[static_cast<std::size_t>(n)] =
                buf[static_cast<std::size_t>(n)] * window[static_cast<std::size_t>(n)];
        std::fill(fftBuf.begin() + kFftSize, fftBuf.end(), 0.0f);

        fft.performRealOnlyForwardTransform(fftBuf.data());  // in place, packed complex

        for (int k = 0; k < kNumBins; ++k) {
            const float re = fftBuf[static_cast<std::size_t>(2 * k)];
            const float im = fftBuf[static_cast<std::size_t>(2 * k + 1)];
            power[static_cast<std::size_t>(k)] += re * re + im * im;
        }
    }

    void boxcarSmoothPower() {
        prefix[0] = 0.0;
        for (int k = 0; k < kNumBins; ++k)
            prefix[static_cast<std::size_t>(k) + 1] =
                prefix[static_cast<std::size_t>(k)] + static_cast<double>(power[static_cast<std::size_t>(k)]);

        for (int k = 0; k < kNumBins; ++k) {
            const int w = halfWidth[static_cast<std::size_t>(k)];
            const int lo = std::max(0, k - w);
            const int hi = std::min(kNumBins - 1, k + w);
            const double sum = prefix[static_cast<std::size_t>(hi) + 1] - prefix[static_cast<std::size_t>(lo)];
            scratch[static_cast<std::size_t>(k)] = static_cast<float>(sum / static_cast<double>(hi - lo + 1));
        }
        std::swap(power, scratch);
    }

    static float powerToDb(float p) {
        return p > 0.0f ? std::max(kFloorDb, 10.0f * std::log10(p)) : kFloorDb;
    }

    juce::dsp::FFT fft;
    double sampleRate = 0.0;
    float releaseP = 0.2f;
    int activeChannels = 0;

    std::vector<float> window;
    std::array<std::vector<float>, kMaxChannels> circ{};
    std::array<float, kMaxChannels> dcX{};
    std::array<float, kMaxChannels> dcY{};

    std::vector<float> fftBuf;
    std::vector<float> power;
    std::vector<float> scratch;
    std::vector<double> prefix;
    std::vector<float> binDbBuf;
    std::vector<float> rawDbBuf;
    std::vector<float> state;
    std::vector<int> halfWidth;
    std::vector<float> tiltDb;
};
