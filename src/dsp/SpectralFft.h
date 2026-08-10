#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

/**
    Hand-rolled radix-2 real FFT, double precision, JUCE-free, header-only.

    WHY WE ROLL OUR OWN: the spectral node runs off a *fixed* frame size (N=1024)
    at ~172 frames/s. A plain iterative radix-2 Cooley-Tukey is trivially inside
    budget there (one N-point transform is N*log2(N) = 1024*10 ≈ 10k complex
    butterflies; forward+inverse per hop per channel is ~4 transforms, so well
    under 1e5 flops per hop — nothing next to the median/interp work), and rolling
    our own keeps this node JUCE-free like the rest of src/dsp/ (the contract) and
    lets the console test reproduce numpy's rfft/irfft to the bit budget the
    fixtures need. A split-radix or real-optimised kernel would be faster but buys
    nothing we can measure here, so we keep it simple and obviously-correct.

    NORMALISATION — matches numpy exactly (this is load-bearing, the fixtures are
    numpy rfft/irfft): forward rfft is UNNORMALISED, inverse irfft divides by N.
      forward(x)[k] = sum_n x[n] * exp(-2*pi*i*k*n/N),  k = 0..N/2   (the half
                      spectrum; the upper half is the conjugate mirror of a real
                      signal and is not stored).
      inverse(X)[n] = (1/N) * sum_k Xfull[k] * exp(+2*pi*i*k*n/N),  Xfull rebuilt
                      from the stored half by Hermitian symmetry Xfull[N-k] =
                      conj(Xfull[k]). Output is real; the tiny imaginary residue
                      is discarded (numpy's rfft-based irfft does the same).
    So irfft(rfft(x)) == x to ~1e-13 (round-trip test), and forward matches numpy
    rfft to ~1e-9 (pocketfft-vs-radix-2 rounding only).

    LIFECYCLE (src/dsp contract): prepare(N) does ALL allocation (bit-reversal
    table, twiddle table, scratch). forward()/inverse() allocate nothing — they
    reuse the preallocated scratch. Single-threaded audio use only (one shared
    scratch); the node calls forward/inverse sequentially per channel.
*/
class SpectralFft {
public:
    /** Off-thread. n must be a power of two. Precomputes bit-reversal indices and
        the negative-angle twiddle table; sizes the transform scratch. */
    void prepare(int n) {
        size_ = n;
        // log2(N): N is a power of two, so this is just the bit count.
        logN_ = 0;
        while ((1 << logN_) < n) ++logN_;

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
    }

    /** Real length-N input -> half spectrum (re/im, length N/2+1). Unnormalised,
        matching numpy rfft. outRe/outIm must each hold at least N/2+1 doubles. */
    void forward(const double* in, double* outRe, double* outIm) {
        const int n = size_;
        // Load the real input in bit-reversed order (imag = 0).
        for (int i = 0; i < n; ++i) {
            re_[static_cast<std::size_t>(i)] = in[bitrev_[static_cast<std::size_t>(i)]];
            im_[static_cast<std::size_t>(i)] = 0.0;
        }
        butterflies(/*inverse=*/false);
        const int half = n / 2;
        for (int b = 0; b <= half; ++b) {
            outRe[b] = re_[static_cast<std::size_t>(b)];
            outIm[b] = im_[static_cast<std::size_t>(b)];
        }
    }

    /** Half spectrum (re/im, length N/2+1) -> real length-N output. Divides by N,
        matching numpy irfft. `out` must hold at least N doubles. */
    void inverse(const double* inRe, const double* inIm, double* out) {
        const int n = size_;
        const int half = n / 2;
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
    }

    int size() const { return size_; }

private:
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

    int size_ = 0;
    int logN_ = 0;
    std::vector<int> bitrev_;
    std::vector<double> twRe_, twIm_;
    std::vector<double> re_, im_;  // transform scratch (allocated in prepare)
};
