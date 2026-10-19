#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

// On macOS the double-precision Accelerate/vDSP real FFT (vDSP_fft_zripD) is the
// production path: ~5x faster than the hand-rolled radix-2 below at N=1024, with
// a round-trip identity error at the double noise floor (~3e-16) and a forward
// spectrum that matches the scalar reference to ~1e-13 (measured, scratchpad/
// vdsp_fft_bench.cpp). The scalar radix-2 path stays as the fallback on every
// other platform and remains the numpy-matched reference implementation the
// fixtures were validated against. Define SPECTRALFFT_FORCE_SCALAR to compile the
// scalar path on macOS too (used by the equivalence scratch test).
#if defined(__APPLE__) && !defined(SPECTRALFFT_FORCE_SCALAR)
    #define SPECTRALFFT_USE_VDSP 1
    #include <Accelerate/Accelerate.h>
#else
    #define SPECTRALFFT_USE_VDSP 0
#endif

/**
    Real FFT, double precision, JUCE-free, header-only. Public interface and
    scaling conventions are IDENTICAL across both backends (this is load-bearing;
    the spectral fixtures are numpy rfft/irfft):

    NORMALISATION — matches numpy exactly: forward rfft is UNNORMALISED, inverse
    irfft divides by N.
      forward(x)[k] = sum_n x[n] * exp(-2*pi*i*k*n/N),  k = 0..N/2   (the half
                      spectrum; the upper half is the conjugate mirror of a real
                      signal and is not stored).
      inverse(X)[n] = (1/N) * sum_k Xfull[k] * exp(+2*pi*i*k*n/N),  Xfull rebuilt
                      from the stored half by Hermitian symmetry.
    So irfft(rfft(x)) == x to ~1e-13 (round-trip test), and forward matches numpy
    rfft to ~1e-9 (rounding only).

    LIFECYCLE (src/dsp contract): prepare(N) does ALL allocation — the vDSP setup
    (vDSP_create_fftsetupD allocates; destroyed in the destructor) plus the split
    scratch on the vDSP path, or the bit-reversal/twiddle/scratch tables on the
    scalar path. forward()/inverse() allocate nothing. Single-threaded audio use
    only (one shared scratch); the node calls forward/inverse sequentially per
    channel. Owns the vDSP setup, so it is non-copyable and non-movable — the
    node holds it by value and is never copied/moved after prepare().

    WHY DOUBLE (both paths): a float real FFT injects ~float-epsilon relative
    error into every magnitude, which is far above the 1e-9/1.2e-8 fixture gates
    once it propagates through the exceed/knee dB math; the float vDSP path was
    measured and REJECTED for precision (scratchpad/vdsp_fft_bench.cpp).
*/
class SpectralFft {
public:
#if SPECTRALFFT_USE_VDSP
    SpectralFft() = default;
    ~SpectralFft() {
        if (setupD_ != nullptr) vDSP_destroy_fftsetupD(setupD_);
    }
    // Owns a raw vDSP setup; forbid copy/move (the node never copies/moves it).
    SpectralFft(const SpectralFft&) = delete;
    SpectralFft& operator=(const SpectralFft&) = delete;
    SpectralFft(SpectralFft&&) = delete;
    SpectralFft& operator=(SpectralFft&&) = delete;
#endif

    /** Off-thread. n must be a power of two. Allocates all scratch/tables. */
    void prepare(int n) {
        size_ = n;
        // log2(N): N is a power of two, so this is just the bit count.
        logN_ = 0;
        while ((1 << logN_) < n) ++logN_;

#if SPECTRALFFT_USE_VDSP
        if (setupD_ != nullptr) vDSP_destroy_fftsetupD(setupD_);
        setupD_ = vDSP_create_fftsetupD(static_cast<vDSP_Length>(logN_), kFFTRadix2);
        const std::size_t half = static_cast<std::size_t>(n / 2);
        splitRe_.assign(half, 0.0);
        splitIm_.assign(half, 0.0);
#else
        const std::size_t un = static_cast<std::size_t>(n);
        bitrev_.assign(un, 0);
        for (std::size_t i = 0; i < un; ++i)
            bitrev_[i] = reverseBits(static_cast<int>(i), logN_);

        // Twiddles for a forward DIT: tw[t] = exp(-2*pi*i * t / N), t = 0..N/2-1.
        // The inverse transform reuses these conjugated (imag negated).
        twRe_.assign(un / 2, 0.0);
        twIm_.assign(un / 2, 0.0);
        for (std::size_t t = 0; t < un / 2; ++t) {
            const double ang = -2.0 * kPi * static_cast<double>(t) / static_cast<double>(n);
            twRe_[t] = std::cos(ang);
            twIm_[t] = std::sin(ang);
        }

        re_.assign(un, 0.0);
        im_.assign(un, 0.0);
#endif
    }

    /** Real length-N input -> half spectrum (re/im, length N/2+1). Unnormalised,
        matching numpy rfft. outRe/outIm must each hold at least N/2+1 doubles. */
    void forward(const double* in, double* outRe, double* outIm) {
        const int n = size_;
        const int half = n / 2;
#if SPECTRALFFT_USE_VDSP
        // Pack the N reals as N/2 interleaved complex, then in-place real FFT.
        DSPDoubleSplitComplex split{splitRe_.data(), splitIm_.data()};
        vDSP_ctozD(reinterpret_cast<const DSPDoubleComplex*>(in), 2, &split, 1,
                   static_cast<vDSP_Length>(half));
        vDSP_fft_zripD(setupD_, &split, 1, static_cast<vDSP_Length>(logN_),
                       kFFTDirection_Forward);
        // vDSP packs the real forward FFT at 2x the mathematical DFT, with DC in
        // realp[0] and Nyquist in imagp[0]; its imag already follows numpy's
        // exp(-i) sign convention (verified vs the scalar reference to ~1e-13,
        // scratchpad/vdsp_fft_verify.cpp), so only the 0.5 rescale is needed.
        outRe[0] = 0.5 * splitRe_[0];
        outIm[0] = 0.0;
        outRe[static_cast<std::size_t>(half)] = 0.5 * splitIm_[0];
        outIm[static_cast<std::size_t>(half)] = 0.0;
        for (int k = 1; k < half; ++k) {
            outRe[k] = 0.5 * splitRe_[static_cast<std::size_t>(k)];
            outIm[k] = 0.5 * splitIm_[static_cast<std::size_t>(k)];
        }
#else
        // Load the real input in bit-reversed order (imag = 0).
        for (int i = 0; i < n; ++i) {
            re_[static_cast<std::size_t>(i)] = in[bitrev_[static_cast<std::size_t>(i)]];
            im_[static_cast<std::size_t>(i)] = 0.0;
        }
        butterflies(/*inverse=*/false);
        for (int b = 0; b <= half; ++b) {
            outRe[b] = re_[static_cast<std::size_t>(b)];
            outIm[b] = im_[static_cast<std::size_t>(b)];
        }
#endif
    }

    /** Half spectrum (re/im, length N/2+1) -> real length-N output. Divides by N,
        matching numpy irfft. `out` must hold at least N doubles. */
    void inverse(const double* inRe, const double* inIm, double* out) {
        const int n = size_;
        const int half = n / 2;
#if SPECTRALFFT_USE_VDSP
        // Repack the half spectrum into vDSP's packed real format (DC in realp[0],
        // Nyquist in imagp[0]) at 1x scale, matching the forward packing above;
        // the fwd(2x)->inv round trip then yields N*x, so the final 1/N gives
        // numpy irfft exactly.
        DSPDoubleSplitComplex split{splitRe_.data(), splitIm_.data()};
        splitRe_[0] = inRe[0];
        splitIm_[0] = inRe[static_cast<std::size_t>(half)];
        for (int k = 1; k < half; ++k) {
            splitRe_[static_cast<std::size_t>(k)] = inRe[k];
            splitIm_[static_cast<std::size_t>(k)] = inIm[k];
        }
        vDSP_fft_zripD(setupD_, &split, 1, static_cast<vDSP_Length>(logN_),
                       kFFTDirection_Inverse);
        vDSP_ztocD(&split, 1, reinterpret_cast<DSPDoubleComplex*>(out), 2,
                   static_cast<vDSP_Length>(half));
        const double invN = 1.0 / static_cast<double>(n);
        for (int i = 0; i < n; ++i) out[i] *= invN;
#else
        // Rebuild the full spectrum by Hermitian symmetry directly in bit-reversed
        // order: index i pulls from the natural-order bin k = bitrev_[i]. For
        // k <= N/2 use the stored half; for k > N/2 use conj of bin N-k.
        for (int i = 0; i < n; ++i) {
            const int k = bitrev_[static_cast<std::size_t>(i)];
            if (k <= half) {
                re_[static_cast<std::size_t>(i)] = inRe[k];
                im_[static_cast<std::size_t>(i)] = inIm[k];
            } else {
                const int kk = n - k;  // 1..N/2-1
                re_[static_cast<std::size_t>(i)] = inRe[kk];
                im_[static_cast<std::size_t>(i)] = -inIm[kk];
            }
        }
        butterflies(/*inverse=*/true);
        const double invN = 1.0 / static_cast<double>(n);
        for (int i = 0; i < n; ++i)
            out[i] = re_[static_cast<std::size_t>(i)] * invN;  // real part only
#endif
    }

    int size() const { return size_; }

private:
    int size_ = 0;
    int logN_ = 0;

#if SPECTRALFFT_USE_VDSP
    FFTSetupD setupD_ = nullptr;
    std::vector<double> splitRe_, splitIm_;  // packed-real scratch (prepare-sized)
#else
    static constexpr double kPi = 3.14159265358979323846;

    static int reverseBits(int x, int bits) {
        int r = 0;
        for (int b = 0; b < bits; ++b) {
            r = (r << 1) | (x & 1);
            x >>= 1;
        }
        return r;
    }

    /** In-place iterative radix-2 DIT on re_/im_ (already bit-reversed). Natural
        order out. `inverse` conjugates the twiddles (+angle instead of -). */
    void butterflies(bool inverse) {
        const int n = size_;
        for (int len = 2; len <= n; len <<= 1) {
            const int halfL = len >> 1;
            const int tstep = n / len;
            for (int i = 0; i < n; i += len) {
                for (int j = 0; j < halfL; ++j) {
                    const std::size_t t = static_cast<std::size_t>(j * tstep);
                    const double wr = twRe_[t];
                    const double wi = inverse ? -twIm_[t] : twIm_[t];
                    const std::size_t a = static_cast<std::size_t>(i + j);
                    const std::size_t b = a + static_cast<std::size_t>(halfL);
                    const double xr = re_[b] * wr - im_[b] * wi;
                    const double xi = re_[b] * wi + im_[b] * wr;
                    re_[b] = re_[a] - xr;
                    im_[b] = im_[a] - xi;
                    re_[a] += xr;
                    im_[a] += xi;
                }
            }
        }
    }

    std::vector<int> bitrev_;
    std::vector<double> twRe_, twIm_;
    std::vector<double> re_, im_;  // transform scratch (allocated in prepare)
#endif
};
