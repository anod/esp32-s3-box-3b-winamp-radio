// ============================================================
// Lock-free Single-Producer Single-Consumer Ring Buffer
// For transferring PCM audio from I2S reader to A2DP callback.
// ============================================================

#pragma once

#include <cstdint>
#include <cstring>
#include <atomic>

class PcmRingBuffer {
public:
    // capacity must be power of 2
    PcmRingBuffer(size_t capacityFrames)
        : m_capacity(capacityFrames)
        , m_mask(capacityFrames - 1)
        , m_head(0)
        , m_tail(0)
        , m_underruns(0)
    {
        // Each frame = 2 × int16_t (stereo)
        m_buf = new int16_t[capacityFrames * 2];
        memset(m_buf, 0, capacityFrames * 2 * sizeof(int16_t));
    }

    ~PcmRingBuffer() { delete[] m_buf; }

    // Producer: write stereo frames. Returns frames actually written.
    size_t write(const int16_t* data, size_t frames) {
        size_t head = m_head.load(std::memory_order_relaxed);
        size_t tail = m_tail.load(std::memory_order_acquire);
        size_t avail = m_capacity - (head - tail);
        if (frames > avail) frames = avail;
        for (size_t i = 0; i < frames; i++) {
            size_t idx = ((head + i) & m_mask) * 2;
            m_buf[idx]     = data[i * 2];
            m_buf[idx + 1] = data[i * 2 + 1];
        }
        m_head.store(head + frames, std::memory_order_release);
        return frames;
    }

    // Consumer: read stereo frames. Fills silence on underrun. Returns frames read.
    size_t read(int16_t* data, size_t frames) {
        size_t head = m_head.load(std::memory_order_acquire);
        size_t tail = m_tail.load(std::memory_order_relaxed);
        size_t available = head - tail;
        size_t toRead = (frames < available) ? frames : available;

        for (size_t i = 0; i < toRead; i++) {
            size_t idx = ((tail + i) & m_mask) * 2;
            data[i * 2]     = m_buf[idx];
            data[i * 2 + 1] = m_buf[idx + 1];
        }
        // Fill remainder with silence
        if (toRead < frames) {
            memset(data + toRead * 2, 0, (frames - toRead) * 2 * sizeof(int16_t));
            m_underruns.fetch_add(1, std::memory_order_relaxed);
        }
        m_tail.store(tail + toRead, std::memory_order_release);
        return toRead;
    }

    size_t available() const {
        return m_head.load(std::memory_order_acquire) -
               m_tail.load(std::memory_order_relaxed);
    }

    size_t free() const {
        return m_capacity - available();
    }

    uint32_t underruns() const {
        return m_underruns.load(std::memory_order_relaxed);
    }

    void resetUnderruns() {
        m_underruns.store(0, std::memory_order_relaxed);
    }

private:
    int16_t*           m_buf;
    const size_t       m_capacity;
    const size_t       m_mask;
    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
    std::atomic<uint32_t> m_underruns;
};
