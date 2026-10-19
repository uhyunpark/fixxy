#pragma once

#include "SpectralFft.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

// Static primitives ported from the independently verified minimum_phase_fir,
// FirKernel, and CausalPartitionedFir in proto/spectral_parity.py. No dynamic
// transition policy is implied; SpectralTransition supplies the verified policy.
// These are our DSP approximation, not a claim
// about soothe's implementation. All prepare/destruction calls are off-thread.
class SpectralFirSynthesis {
public:
    // Positive target bins must already be sampled on this exact design grid.
    // Supported contract: even power-of-two grid, 1 <= tapCount <= designSize.
    void prepare(std::size_t designSize = 16384, std::size_t tapCount = 4096) {
        assert(designSize >= 2 && (designSize & (designSize-1)) == 0);
        assert(designSize <= static_cast<std::size_t>(std::numeric_limits<int>::max()));
        assert(tapCount > 0 && tapCount <= designSize);
        size_ = designSize;
        taps_ = tapCount;
        fft_.prepare(static_cast<int>(size_));
        real_.assign(size_/2+1, 0.0);
        imag_.assign(size_/2+1, 0.0);
        time_.assign(size_, 0.0);
    }

    // No allocation after prepare, but substantial bounded FFT work: schedule
    // off the audio thread. Input: size/2+1 finite reduction-dB values. Output:
    // tapCount doubles. No window or normalization after causal truncation.
    void minimumPhase(const double* reductionDb, double* taps) {
        bool unity = true;
        for (std::size_t k = 0; k < real_.size(); ++k) {
            const double reduction = std::clamp(reductionDb[k], 0.0, 40.0);
            unity = unity && reduction == 0.0;
            // Algebraically log(10^(-R/20)); avoids two per-bin transcendentals.
            real_[k] = -reduction * (std::numbers::ln10/20.0);
            imag_[k] = 0.0;
        }
        if (unity) {
            std::fill_n(taps, taps_, 0.0);
            taps[0] = 1.0;
            return;
        }
        // Real inverse FFT supplies the explicit real-even log mirror. The
        // causal cepstrum retains DC/Nyquist and doubles positive quefrencies.
        fft_.inverse(real_.data(), imag_.data(), time_.data());
        for (std::size_t n = 1; n < size_/2; ++n) time_[n] *= 2.0;
        std::fill(time_.begin()+static_cast<std::ptrdiff_t>(size_/2+1), time_.end(), 0.0);
        fft_.forward(time_.data(), real_.data(), imag_.data());
        for (std::size_t k = 0; k < real_.size(); ++k) {
            const double magnitude = std::exp(real_[k]);
            const double phase = imag_[k];
            real_[k] = magnitude * std::cos(phase);
            imag_[k] = magnitude * std::sin(phase);
        }
        fft_.inverse(real_.data(), imag_.data(), time_.data());
        std::copy_n(time_.data(), taps_, taps);
    }

    // Retain the causal-cepstrum log BEFORE exponential/truncation for phase
    // admission. Positive bins only; the full spectrum is Hermitian. Two FFTs.
    void causalLog(const double* reductionDb, double capDb, double* logReal, double* logImag) {
        for (std::size_t k = 0; k < real_.size(); ++k) {
            real_[k] = -std::clamp(reductionDb[k], 0.0, capDb)*(std::numbers::ln10/20.0);
            imag_[k] = 0.0;
        }
        fft_.inverse(real_.data(), imag_.data(), time_.data());
        for (std::size_t n = 1; n < size_/2; ++n) time_[n] *= 2.0;
        std::fill(time_.begin()+static_cast<std::ptrdiff_t>(size_/2+1), time_.end(), 0.0);
        fft_.forward(time_.data(), logReal, logImag);
        logImag[0] = logImag[size_/2] = 0.0;
    }

    // One further transform synthesizes the admitted endpoint, not the target.
    void fromCausalLog(const double* logReal, const double* logImag, double* taps) {
        for (std::size_t k = 0; k < real_.size(); ++k) {
            const double magnitude = std::exp(logReal[k]);
            real_[k] = magnitude*std::cos(logImag[k]);
            imag_[k] = magnitude*std::sin(logImag[k]);
        }
        imag_[0] = imag_.back() = 0.0;
        fft_.inverse(real_.data(), imag_.data(), time_.data());
        std::copy_n(time_.data(), taps_, taps);
    }

private:
    std::size_t size_ = 0, taps_ = 0;
    SpectralFft fft_;
    std::vector<double> real_, imag_, time_;
};

// Immutable while rendered. refresh() is permitted only for an inactive bank,
// immediately followed by replaceAtBoundary() on every referring renderer.
class SpectralFirKernel {
public:
    static constexpr std::size_t partition = 64;
    static constexpr std::size_t bins = partition+1;

    void prepare(const double* taps, std::size_t count) {
        assert(count > 0);
        partitions_ = (count+partition-1)/partition;
        real_.assign(partitions_*bins, 0.0);
        imag_.assign(partitions_*bins, 0.0);
        tapCount_ = count;
        fft_.prepare(static_cast<int>(2*partition));
        refresh(taps);
    }

    // Allocation-free coefficient refresh at the prepared fixed tap capacity.
    void refresh(const double* taps) {
        head_.fill(0.0);
        std::copy_n(taps, std::min(tapCount_, partition), head_.data());
        std::array<double, 2*partition> block{};
        for (std::size_t j = 0; j < partitions_; ++j) {
            block.fill(0.0);
            std::copy_n(taps+j*partition, std::min(partition, tapCount_-j*partition), block.data());
            fft_.forward(block.data(), real_.data()+j*bins, imag_.data()+j*bins);
        }
    }

    std::size_t partitions() const { return partitions_; }
    const std::array<double, partition>& head() const { return head_; }
    std::span<const double> real(std::size_t j) const { return {real_.data()+j*bins, bins}; }
    std::span<const double> imag(std::size_t j) const { return {imag_.data()+j*bins, bins}; }

private:
    std::size_t partitions_ = 0, tapCount_ = 0;
    SpectralFft fft_;
    std::array<double, partition> head_{};
    std::vector<double> real_, imag_;
};

class SpectralFirConvolver {
public:
    // Kernel must outlive this renderer and must not change until it is no
    // longer rendered, except the explicit partition-boundary replacement below.
    void prepare(const SpectralFirKernel& kernel) {
        assert(kernel.partitions() > 0);
        kernel_ = &kernel;
        capacity_ = kernel.partitions()+1;
        fft_.prepare(static_cast<int>(2*P));
        historyReal_.assign(capacity_*B, 0.0);
        historyImag_.assign(capacity_*B, 0.0);
        reset();
    }

    void reset() {
        std::fill(historyReal_.begin(), historyReal_.end(), 0.0);
        std::fill(historyImag_.begin(), historyImag_.end(), 0.0);
        headHistory_.fill(0.0);
        input_.fill(0.0);
        tail_.fill(0.0);
        overlap_.fill(0.0);
        position_ = 0;
        spectrumSlot_ = 0;
    }

    // Identical partition count, at a P boundary only. Rebuild BOTH previous
    // overlap and current tail from this lane's retained J+1 input spectra.
    // Carrying old-kernel overlap across activation gives incorrect history.
    void replaceAtBoundary(const SpectralFirKernel& kernel) {
        assert(position_ == 0 && kernel.partitions()+1 == capacity_);
        kernel_ = &kernel;
        computeTail(1);
        std::copy_n(time_.data()+P, P, overlap_.data());
        prepareTail();
    }

    // Mono lane, double precision, in-place supported. Zero samples permits
    // null pointers. Internal partition boundaries never follow host chunks.
    void process(const double* input, double* output, std::size_t count) {
        for (std::size_t n = 0; n < count; ++n) {
            const double sample = input[n];
            headHistory_[position_] = sample;
            input_[position_] = sample;
            double head = 0.0;
            for (std::size_t r = 0; r < P; ++r)
                head += kernel_->head()[r] * headHistory_[(position_+P-r) & (P-1)];
            output[n] = head + tail_[position_];
            if (++position_ == P) {
                fft_.forward(input_.data(), historyReal_.data()+spectrumSlot_*B,
                             historyImag_.data()+spectrumSlot_*B);
                spectrumSlot_ = (spectrumSlot_+1) % capacity_;
                position_ = 0;
                prepareTail();
            }
        }
    }

private:
    static constexpr std::size_t P = SpectralFirKernel::partition;
    static constexpr std::size_t B = SpectralFirKernel::bins;

    void computeTail(std::size_t blockLag) {
        sumReal_.fill(0.0);
        sumImag_.fill(0.0);
        for (std::size_t j = 1; j < kernel_->partitions(); ++j) {
            // Slot is the next block b, so j reads the completed input b-j.
            // All negative-time slots start zero; wrapping uses J+1 spectra.
            const auto slot = (spectrumSlot_+capacity_-j-blockLag) % capacity_;
            const double* xr = historyReal_.data()+slot*B;
            const double* xi = historyImag_.data()+slot*B;
            const auto fr = kernel_->real(j), fi = kernel_->imag(j);
            for (std::size_t k = 0; k < B; ++k) {
                sumReal_[k] += fr[k]*xr[k] - fi[k]*xi[k];
                sumImag_[k] += fr[k]*xi[k] + fi[k]*xr[k];
            }
        }
        fft_.inverse(sumReal_.data(), sumImag_.data(), time_.data());
    }

    void prepareTail() {
        computeTail(0);
        for (std::size_t r = 0; r < P; ++r) {
            tail_[r] = time_[r] + overlap_[r];
            overlap_[r] = time_[P+r];
        }
    }

    const SpectralFirKernel* kernel_ = nullptr;
    std::size_t capacity_ = 0, position_ = 0, spectrumSlot_ = 0;
    SpectralFft fft_;
    std::vector<double> historyReal_, historyImag_;
    std::array<double, P> headHistory_{}, tail_{}, overlap_{};
    std::array<double, 2*P> input_{}, time_{};
    std::array<double, B> sumReal_{}, sumImag_{};
};
