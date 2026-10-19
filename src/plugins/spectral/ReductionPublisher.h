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

    Atomic payload makes concurrent copies legal in the C++ memory model;
    checking a sequence afterwards does not make plain-float races safe.
    The writer's release fence precedes relaxed payload stores. If a reader
    observes any of those stores, its acquire fence orders the earlier odd
    sequence before the final sequence check, which then rejects that copy.
    The initial acquire of a completed even sequence exposes that whole frame.
    Storage is fixed; publishing performs N atomic stores, with no retry loop.
*/
template <int N>
class ReductionPublisher {
    static_assert(std::atomic<float>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
public:
    /** AUDIO THREAD. Publish a snapshot of src[0..n) (rest zero-filled). */
    void write(const float* src, int n) noexcept {
        const int m = n < N ? n : N;
        const std::uint32_t sequence = seq_.load(std::memory_order_relaxed);
        seq_.store(sequence + 1u, std::memory_order_relaxed);  // single writer: odd
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < m; ++i)
            buf_[static_cast<std::size_t>(i)].store(src[i], std::memory_order_relaxed);
        for (int i = m; i < N; ++i)
            buf_[static_cast<std::size_t>(i)].store(0.0f, std::memory_order_relaxed);
        seq_.store(sequence + 2u, std::memory_order_release);  // even: complete
    }

    /** MESSAGE THREAD. Copy the latest complete snapshot into dst[0..n).
        Returns false if it could not get a clean read (writer very hot) — the
        caller should keep whatever it drew last frame. */
    bool read(float* dst, int n) const noexcept {
        const int m = n < N ? n : N;
        std::array<float, N> snapshot{};
        for (int attempt = 0; attempt < 8; ++attempt) {
            const std::uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u)
                continue;  // mid-write
            for (int i = 0; i < m; ++i)
                snapshot[static_cast<std::size_t>(i)] = buf_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) {
                for (int i = 0; i < m; ++i)
                    dst[i] = snapshot[static_cast<std::size_t>(i)];
                return true;  // no write straddled the copy
            }
        }
        return false;
    }

private:
    std::atomic<std::uint32_t> seq_{0};
    std::array<std::atomic<float>, N> buf_{};
};
