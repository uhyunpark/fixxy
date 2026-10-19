#pragma once

#include <array>
#include <cstring>
#include <vector>

#include <juce_core/juce_core.h>

/**
    Single-producer / single-consumer lock-free tap between the audio thread
    (producer, in processBlock) and the message thread (consumer, the analyzer's
    30 Hz timer). One juce::AbstractFifo governs a pair of per-channel float
    buffers in lockstep — the same read/write index pair addresses both channels,
    so L and R are always frame-aligned.

    Real-time contract (the ONE audio-thread addition of Stage 2):
      - write() calls NOTHING that allocates, locks, or blocks. getFreeSpace(),
        prepareToWrite() and finishedWrite() are pure atomic-index arithmetic.
        The per-channel std::vectors are sized ONCE in prepare() (off-thread) and
        never touched again on the audio thread.
      - On overflow (UI stalled, not draining) we write only what fits and DROP
        the rest. Never block, never grow. A dropped block is a momentary gap in
        the analyzer, never an audio glitch.

    Capacity is sized for the highest supported sample rate and fixed for the
    processor's lifetime, so a mid-session sample-rate change re-prepares WITHOUT
    reallocating the storage the message-thread reader may be touching.
*/
class SpectrumFifo {
public:
    /** Off audio thread. Allocates once; on a re-prepare with the same capacity
        it only resets the atomic indices (no reallocation under the reader). */
    void prepare(int capacity, int numChannels) {
        nCh = juce::jlimit(1, 2, numChannels);
        if (capacity != allocatedCapacity) {
            allocatedCapacity = capacity;
            for (auto& b : buffers)
                b.assign(static_cast<std::size_t>(capacity), 0.0f);
            fifo.setTotalSize(capacity);
        }
        fifo.reset();
    }

    int getCapacity() const { return allocatedCapacity; }
    int getNumChannels() const { return nCh; }

    /** AUDIO THREAD. Push up to numSamples frames; drop whatever does not fit.
        No allocation, no lock, no I/O — bounded by numSamples. */
    void write(const float* const* channels, int chIn, int numSamples) {
        const int n = juce::jmin(numSamples, fifo.getFreeSpace());  // DROP on overflow
        if (n <= 0)
            return;
        int s1, sz1, s2, sz2;
        fifo.prepareToWrite(n, s1, sz1, s2, sz2);
        for (int ch = 0; ch < nCh; ++ch) {
            const float* src = channels[ch < chIn ? ch : 0];
            float* dst = buffers[static_cast<std::size_t>(ch)].data();
            if (sz1 > 0)
                std::memcpy(dst + s1, src, sizeof(float) * static_cast<std::size_t>(sz1));
            if (sz2 > 0)
                std::memcpy(dst + s2, src + sz1, sizeof(float) * static_cast<std::size_t>(sz2));
        }
        fifo.finishedWrite(sz1 + sz2);
    }

    /** MESSAGE THREAD. Read up to maxFrames of all available frames into dst
        (dst[ch] must hold at least maxFrames). Returns frames read. */
    int read(float* const* dst, int dstNumCh, int maxFrames) {
        const int n = juce::jmin(fifo.getNumReady(), maxFrames);
        if (n <= 0)
            return 0;
        int s1, sz1, s2, sz2;
        fifo.prepareToRead(n, s1, sz1, s2, sz2);
        const int copyCh = juce::jmin(nCh, dstNumCh);
        for (int ch = 0; ch < copyCh; ++ch) {
            const float* src = buffers[static_cast<std::size_t>(ch)].data();
            if (sz1 > 0)
                std::memcpy(dst[ch], src + s1, sizeof(float) * static_cast<std::size_t>(sz1));
            if (sz2 > 0)
                std::memcpy(dst[ch] + sz1, src + s2, sizeof(float) * static_cast<std::size_t>(sz2));
        }
        fifo.finishedRead(sz1 + sz2);
        return n;
    }

private:
    juce::AbstractFifo fifo{1};
    std::array<std::vector<float>, 2> buffers{};
    int allocatedCapacity = 0;
    int nCh = 2;
};
