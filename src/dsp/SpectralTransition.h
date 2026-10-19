#pragma once

#include "SpectralFir.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

// Port of VERIFIED proto/spectral_transition.py with its FIXED 0.01 rad reserve.
// The nominal phase limit alone was rejected. Bounds have dense/refined numeric
// evidence for the tested curves, not a mathematical continuum certificate.
// One mono lane. Its owner splits callbacks at remaining() and supplies the newest
// target once via beginHop(). No queue, target scheduler, or worker is hidden here.
class SpectralTransition {
public:
    static constexpr double phaseLimit = 0.19179375027524279;
    static constexpr double phaseErrorReserve = 0.01;
    static constexpr double admissionLimit = phaseLimit-phaseErrorReserve;

    void prepare(std::size_t designSize, std::size_t tapCount, std::size_t hop,
                 const double* initialReduction, double initialCap) {
        assert(hop == 64 || hop == 128 || hop == 256);
        assert(initialCap >= 0.0 && initialCap <= 40.0);
        hop_ = hop;
        synthesis_.prepare(designSize, tapCount);
        logReal_.resize(designSize/2+1); logImag_.resize(designSize/2+1);
        targetReal_.resize(designSize/2+1); targetImag_.resize(designSize/2+1);
        taps_.resize(tapCount);
        synthesis_.causalLog(initialReduction, initialCap, logReal_.data(), logImag_.data());
        synthesis_.fromCausalLog(logReal_.data(), logImag_.data(), taps_.data());
        for (std::size_t i = 0; i < 2; ++i) {
            kernels_[i].prepare(taps_.data(), tapCount);
            banks_[i].prepare(kernels_[i]);
        }
        for (std::size_t q = 0; q < hop; ++q)
            weights_[q] = 0.5-0.5*std::cos(std::numbers::pi*static_cast<double>(q)/static_cast<double>(hop));
        active_ = 0; position_ = hop_; admissions_ = 0;
        currentCap_ = nextCap_ = initialCap;
        alpha_ = 1.0; distance_ = 0.0;
    }

    // Exactly one new IR per completed H. Requests are on the prepared M/2+1
    // grid and already smoothed; cap is the requested 20 ms control smoother.
    // This bounded but substantial update is NOT approved for synchronous audio
    // use merely because it does not allocate; see complete-path timing report.
    bool beginHop(const double* newestReduction, double requestedCap) {
        if (remaining() != 0) return false;
        assert(requestedCap >= 0.0 && requestedCap <= 40.0);
        synthesis_.causalLog(newestReduction, requestedCap, targetReal_.data(), targetImag_.data());
        distance_ = 0.0;
        for (std::size_t k = 0; k < logImag_.size(); ++k)
            distance_ = std::max(distance_, std::abs(targetImag_[k]-logImag_[k]));
        alpha_ = distance_ == 0.0 ? 1.0 : std::min(1.0, admissionLimit/distance_);
        for (std::size_t k = 0; k < logReal_.size(); ++k) {
            logReal_[k] += alpha_*(targetReal_[k]-logReal_[k]);
            logImag_[k] += alpha_*(targetImag_[k]-logImag_[k]);
        }
        logImag_.front() = logImag_.back() = 0.0;
        currentCap_ = nextCap_;
        nextCap_ = (1.0-alpha_)*currentCap_+alpha_*requestedCap;
        synthesis_.fromCausalLog(logReal_.data(), logImag_.data(), taps_.data());
        const auto next = 1-active_;
        kernels_[next].refresh(taps_.data());
        banks_[next].replaceAtBoundary(kernels_[next]);
        position_ = 0; ++admissions_;
        return true;
    }

    // count <= remaining(); caller splits irregular host callbacks accordingly.
    // Both banks always consume the same source history, including zero input.
    // Input/output may alias. Zero-length calls accept null pointers.
    void process(const double* input, double* output, std::size_t count) {
        assert(count <= remaining());
        banks_[active_].process(input, oldOutput_.data(), count);
        banks_[1-active_].process(input, newOutput_.data(), count);
        for (std::size_t n = 0; n < count; ++n) {
            const double t = weights_[position_+n];
            output[n] = (1.0-t)*oldOutput_[n]+t*newOutput_[n];
        }
        position_ += count;
        if (count != 0 && position_ == hop_) active_ = 1-active_;
    }

    std::size_t remaining() const noexcept { return hop_-position_; }
    std::size_t admissions() const noexcept { return admissions_; }
    double alpha() const noexcept { return alpha_; }
    double gridDistance() const noexcept { return distance_; }
    double admittedCap() const noexcept { return nextCap_; }
    double effectiveCap() const noexcept {
        if (position_ == hop_) return nextCap_;
        const double t = weights_[position_];
        return (1.0-t)*currentCap_+t*nextCap_;
    }
    double reduction(std::size_t bin) const noexcept {
        return -logReal_[bin]*(20.0/std::numbers::ln10);
    }
    std::span<const double> admittedTaps() const noexcept { return taps_; }

private:
    SpectralFirSynthesis synthesis_;
    std::array<SpectralFirKernel, 2> kernels_;
    std::array<SpectralFirConvolver, 2> banks_;
    std::vector<double> logReal_, logImag_, targetReal_, targetImag_, taps_;
    std::array<double, 256> weights_{}, oldOutput_{}, newOutput_{};
    std::size_t hop_ = 0, position_ = 0, active_ = 0, admissions_ = 0;
    double currentCap_ = 0.0, nextCap_ = 0.0, alpha_ = 1.0, distance_ = 0.0;
};
