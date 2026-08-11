#pragma once

#include <array>
#include <atomic>
#include <cstdint>

/**
    Single-producer / single-consumer LATEST-SNAPSHOT publisher for the per-bin
    reduction curve (SpectralNode::copyReductionDb). The audio thread writes a
    fresh whole-array snapshot once per block; the editor's 30 Hz timer reads the
    most recent complete one. Unlike SpectrumFifo (a continuous stream), the
    ribbon only ever wants the CURRENT frame, so this is a seqlock, not a queue.

    Atomics live in the plugin layer, never in src/dsp (src/plugins/CLAUDE.md).

    Seqlock protocol (allocation-free, lock-free, bounded = N floats + 2 atomic
    stores per publish):
      writer: seq -> odd (release) ; copy array ; seq -> even (release).
      reader: read seq (acquire, must be even) ; copy array ;
              re-read seq (acquire) ; accept only if unchanged, else retry.
    The reader therefore NEVER uses a torn snapshot — a copy that races a write
    is detected by the sequence mismatch and discarded (the caller keeps its
    previous frame, a momentary UI hold, never wrong data). Storage is fixed for
    the processor's lifetime; write() and read() touch no allocator.
*/
template <int N>
class ReductionPublisher {
public:
    /** AUDIO THREAD. Publish a snapshot of src[0..n) (rest zero-filled). */
    void write(const float* src, int n) noexcept {
        const int m = n < N ? n : N;
        seq_.fetch_add(1, std::memory_order_release);  // -> odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < m; ++i)
            buf_[static_cast<std::size_t>(i)] = src[i];
        for (int i = m; i < N; ++i)
            buf_[static_cast<std::size_t>(i)] = 0.0f;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.fetch_add(1, std::memory_order_release);  // -> even: snapshot complete
    }

    /** MESSAGE THREAD. Copy the latest complete snapshot into dst[0..n).
        Returns false if it could not get a clean read (writer very hot) — the
        caller should keep whatever it drew last frame. */
    bool read(float* dst, int n) const noexcept {
        const int m = n < N ? n : N;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const std::uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u)
                continue;  // mid-write
            std::atomic_thread_fence(std::memory_order_acquire);
            for (int i = 0; i < m; ++i)
                dst[i] = buf_[static_cast<std::size_t>(i)];
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_acquire) == s1)
                return true;  // no write straddled the copy
        }
        return false;
    }

private:
    std::atomic<std::uint32_t> seq_{0};
    std::array<float, N> buf_{};
};
