#pragma once

// Verified proto/spectral_parity.py control primitives. This is our designed
// approximation, not soothe's proprietary algorithm. prepare() owns allocation;
// callers publish/smooth parameters and recompute coefficients outside the sample
// loop. State belongs to each detector channel/bin, never shared between channels.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

namespace spectral_control {
inline constexpr double epsilon = 1e-12;
inline constexpr double maxReductionDb = 40.0;

struct Params {
    double detail = 5.0, detailLo = 0.0, detailHi = 0.0;
    double attack = 5.0, release = 5.0;
    double attackLo = 0.0, attackHi = 0.0;
    double releaseLo = 0.0, releaseHi = 0.0;
};

inline double framePole(double sampleRate, std::size_t hop, double tauSeconds) noexcept {
    return std::exp(-static_cast<double>(hop)/(sampleRate*tauSeconds));
}

struct Coefficients {
    double low, high, detail, widthOct, strength;
    double detectorTau, attackTau, releaseTau;
    double detectorPole, attackPole, releasePole;
};

// Validated inputs: physical frequency >= 0, normalized controls 0..10,
// Detail tilts -5..5, timing tilts -2..2, sampleRate/hop > 0.
inline Coefficients coefficients(double frequency, const Params& p,
                                 double sampleRate, std::size_t hop) noexcept {
    Coefficients c{};
    c.low = 1.0/(1.0+std::pow(frequency/500.0, 4.0));
    c.high = frequency > 0.0 ? 1.0/(1.0+std::pow(2000.0/frequency, 4.0)) : 0.0;
    c.detail = std::clamp(p.detail+p.detailLo*c.low+p.detailHi*c.high, 0.0, 10.0);
    c.widthOct = 0.5*std::pow(2.0, -c.detail/2.0);
    c.strength = std::pow(2.0, (c.detail-5.0)/10.0);
    c.detectorTau = 0.001*std::pow(2.0, c.detail/2.0);
    const double scale = std::clamp(std::sqrt(1000.0/std::max(frequency, 1.0)), 0.25, 4.0);
    c.attackTau = 0.001*std::pow(27.0, p.attack/5.0)*scale
                  *std::pow(2.0, p.attackLo*c.low+p.attackHi*c.high);
    c.releaseTau = 0.005*std::pow(10.0, p.release/5.0)*scale
                   *std::pow(2.0, p.releaseLo*c.low+p.releaseHi*c.high);
    c.detectorPole = framePole(sampleRate, hop, c.detectorTau);
    c.attackPole = framePole(sampleRate, hop, c.attackTau);
    c.releasePole = framePole(sampleRate, hop, c.releaseTau);
    return c;
}

inline double calibratedAmplitude(double fftMagnitude, double analysisWindowSum,
                                  bool dcOrNyquist) noexcept {
    return (dcOrNyquist ? 1.0 : 2.0)*fftMagnitude/analysisWindowSum;
}

inline double exceedance(double amplitude, double background, double weightDb = 0.0) noexcept {
    return 20.0*std::log10((amplitude+epsilon)/(background+epsilon))+weightDb;
}

inline double detectorStep(double exceedDb, double previousDb, double pole) noexcept {
    return pole*previousDb+(1.0-pole)*exceedDb;
}

// Supply active=false for a silent/missing detector frame. Do not pass zero
// exceedance to the knee as a silence substitute: the Soft baseline is nonzero.
inline double softKnee(double exceedDb, double depth, double strength,
                       bool active = true) noexcept {
    if (!active || depth == 0.0) return 0.0;
    const double ceiling = 16.167635300821875*(depth/5.0)*strength;
    return std::clamp(ceiling*0.5*(1.0+std::tanh((exceedDb-18.78301217932193)
                                              /9.010408026990198)), 0.0, maxReductionDb);
}

inline double positiveKnee(double z) noexcept {
    if (z <= -3.0) return 0.0;
    if (z >= 3.0) return z;
    return (z+3.0)*(z+3.0)/12.0;
}

inline double hardReduction(double amplitude, double depth, double weightDb = 0.0,
                            bool active = true) noexcept {
    if (!active || depth == 0.0) return 0.0;
    const double level = 20.0*std::log10(std::max(amplitude, epsilon));
    return std::clamp(0.75*positiveKnee(level+weightDb+6.0+6.0*depth), 0.0, maxReductionDb);
}

struct Ballistics {
    double output = 0.0, release = 0.0;
    double step(double targetDb, double attackPole, double releasePole) noexcept {
        release = std::max(targetDb, releasePole*(release-targetDb)+targetDb);
        output = attackPole*(output-release)+release;
        return output;
    }
    void clamp(double capDb) noexcept {
        output = std::min(output, capDb);
    }
    void reset() noexcept { output = release = 0.0; }
};

inline double smoothCap(double previousDb, double targetDb, double pole) noexcept {
    return pole*previousDb+(1.0-pole)*targetDb;
}

// Inputs are nonnegative calibrated amplitudes; mono retains its one amplitude.
inline std::array<double, 2> link(double a, double b, double fraction) noexcept {
    const double shared = std::sqrt((a*a+b*b)/2.0);
    return {std::sqrt((1.0-fraction)*a*a+fraction*shared*shared),
            std::sqrt((1.0-fraction)*b*b+fraction*shared*shared)};
}

inline std::array<double, 2> focus(double position) noexcept {
    return {1.0-std::max(position, 0.0), 1.0-std::max(-position, 0.0)};
}

// Orthonormal transform is its own inverse; apply to waveforms before FFT,
// not to magnitudes. Mono bypasses this transform.
inline std::array<double, 2> midSide(double a, double b) noexcept {
    return {(a+b)/std::numbers::sqrt2, (a-b)/std::numbers::sqrt2};
}

// Add only enabled bands. Curves sum separately as focus * curveDb.
struct BandpassMask {
    double complement = 1.0, maxFocus = 0.0;
    void add(double mask, double channelFocus = 1.0) noexcept {
        complement *= 1.0-channelFocus*std::clamp(mask, 0.0, 1.0);
        maxFocus = std::max(maxFocus, channelFocus);
    }
    double value() const noexcept {
        return std::clamp(2.0-maxFocus-complement, 0.0, 1.0);
    }
};

// A zero boundary means inactive. Active boundaries must be strictly positive.
inline double rangeMask(double frequency, double lowHz = 0.0, double highHz = 0.0) noexcept {
    constexpr double skirt = 0.125;
    double mask = 1.0;
    if (lowHz > 0.0) {
        const double lower = lowHz*std::pow(2.0, -skirt);
        const double x = std::clamp(std::log2(std::max(frequency, lower)/lower)/skirt, 0.0, 1.0);
        mask *= 0.5-0.5*std::cos(std::numbers::pi*x);
        if (frequency <= 0.0) mask = 0.0;
    }
    if (highHz > 0.0) {
        const double upper = highHz*std::pow(2.0, skirt);
        const double x = std::clamp(std::log2(upper/std::min(std::max(frequency, epsilon), upper))/skirt, 0.0, 1.0);
        mask *= 0.5-0.5*std::cos(std::numbers::pi*x);
    }
    return mask;
}

// Precomputed intervals plus bounded O(B log B) exact median and O(B) box
// smoothing. Median sorts indices once per frame, then maintains order statistics
// with a Fenwick count tree as the +/-0.5 octave window slides across frequencies.
// No approximate quantization or resampling. std::sort has O(B log B) worst-case
// comparison complexity; all scratch storage has fixed size after prepare().
class FrequencyWindows {
public:
    // Frequencies must be strictly increasing, nonnegative, and nonempty;
    // width has the same length. Finite amplitude inputs are nonnegative.
    void prepare(std::span<const double> frequencies, std::span<const double> width) {
        frequencies_.assign(frequencies.begin(), frequencies.end());
        const auto count = frequencies.size();
        medianBounds_.resize(count); smoothBounds_.resize(count);
        order_.resize(count); ranks_.resize(count); tree_.resize(count+1);
        prefix_.resize(count+1);
        for (std::size_t k = 0; k < count; ++k) medianBounds_[k] = bounds(k, 0.5);
        setWidths(width);
    }

    // Run on the owning thread outside the sample loop, after parameter smoothing.
    void setWidths(std::span<const double> width) noexcept {
        for (std::size_t k = 0; k < frequencies_.size(); ++k)
            smoothBounds_[k] = bounds(k, width[k]*0.5);
    }

    // Input/output may alias: the complete prefix is captured before writes.
    void smooth(const double* input, double* output) noexcept {
        prefix_[0] = 0.0;
        for (std::size_t k = 0; k < frequencies_.size(); ++k) prefix_[k+1] = prefix_[k]+input[k];
        for (std::size_t k = 0; k < frequencies_.size(); ++k) {
            const auto [lo, hi] = smoothBounds_[k];
            output[k] = (prefix_[hi]-prefix_[lo])/static_cast<double>(hi-lo);
        }
    }

    // Input/output must not alias; rank lookups retain the original amplitudes.
    void median(const double* input, double* output) noexcept {
        const auto count = frequencies_.size();
        for (std::size_t k = 0; k < count; ++k) order_[k] = k;
        std::sort(order_.begin(), order_.end(), [input](std::size_t a, std::size_t b) {
            return input[a] < input[b] || (input[a] == input[b] && a < b);
        });
        for (std::size_t k = 0; k < count; ++k) ranks_[order_[k]] = k;
        std::fill(tree_.begin(), tree_.end(), 0);
        std::size_t currentLo = 0, currentHi = 0;
        for (std::size_t k = 0; k < count; ++k) {
            const auto [lo, hi] = medianBounds_[k];
            // The right endpoint advances first, including any crossed interval,
            // so removals always refer to an element already in the count tree.
            while (currentHi < hi) update(ranks_[currentHi++], 1);
            while (currentLo < lo) update(ranks_[currentLo++], -1);
            const auto n = hi-lo;
            const double upper = input[order_[select(n/2)]];
            output[k] = (n%2 != 0) ? upper : 0.5*(input[order_[select(n/2-1)]]+upper);
        }
    }

private:
    struct Interval { std::size_t lo, hi; };
    Interval bounds(std::size_t k, double radius) const noexcept {
        const double f = frequencies_[k];
        if (f <= 0.0) return {k, k+1};
        const auto begin = frequencies_.begin(), end = frequencies_.end();
        return {static_cast<std::size_t>(std::lower_bound(begin, end, f*std::pow(2.0, -radius))-begin),
                static_cast<std::size_t>(std::upper_bound(begin, end, f*std::pow(2.0, radius))-begin)};
    }
    void update(std::size_t rank, int delta) noexcept {
        for (std::size_t i = rank+1; i < tree_.size(); i += i & (~i+1)) tree_[i] += delta;
    }
    std::size_t select(std::size_t ordinal) const noexcept {
        std::size_t position = 0, step = 1;
        while (step < tree_.size()) step <<= 1;
        for (step >>= 1; step != 0; step >>= 1) {
            const auto next = position+step;
            if (next < tree_.size() && static_cast<std::size_t>(tree_[next]) <= ordinal) {
                position = next; ordinal -= static_cast<std::size_t>(tree_[next]);
            }
        }
        return position;
    }
    std::vector<double> frequencies_, prefix_;
    std::vector<Interval> medianBounds_, smoothBounds_;
    std::vector<std::size_t> order_, ranks_;
    std::vector<int> tree_;
};
} // namespace spectral_control
