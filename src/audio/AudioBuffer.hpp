#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace quadmapper {

/**
 * @brief Lock-free single-producer / single-consumer ring of float samples
 *
 * Threading model:
 *   Producer (decode thread): push(), writePosition().
 *   Consumer (real-time audio callback): read(), discardTo(), readPosition().
 *   Either side: available(), isEmpty(), isFull(), capacity().
 *
 * The write and read positions count every sample ever pushed / consumed (they never
 * wrap), so a position names one sample of the stream for good: the producer can say
 * "a new segment starts at position P" and the consumer can skip to it. Each side
 * stores only its own position and loads the other's with acquire ordering, so the
 * hot path takes no lock and never allocates.
 *
 * The logical capacity is exactly as requested; the backing array is rounded up to a
 * power of two so wrap-around is a mask.
 */
class AudioBuffer {
public:
    explicit AudioBuffer(size_t capacity = 48000 * 2);  // 1 s of 48 kHz stereo
    ~AudioBuffer() = default;

    AudioBuffer(const AudioBuffer&) = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;

    // --- Producer ---

    /** Push all `count` samples, or nothing (false) when they do not fit. */
    bool push(const float* samples, size_t count);

    /** Samples pushed so far: the position the next push starts at. */
    size_t writePosition() const { return head_.load(std::memory_order_relaxed); }

    // --- Consumer (real-time safe) ---

    /** Copy up to `count` samples out; returns how many were read. */
    size_t read(float* output, size_t count);

    /** Skip forward to `position` (clamped to what has been pushed); never moves back. */
    void discardTo(size_t position);

    /** Samples consumed so far: the position of the next sample read() returns. */
    size_t readPosition() const { return tail_.load(std::memory_order_relaxed); }

    // --- Either side ---

    size_t available() const;
    bool isEmpty() const { return available() == 0; }
    bool isFull() const { return available() >= capacity_; }
    size_t capacity() const { return capacity_; }
    size_t freeSpace() const { return capacity_ - available(); }

private:
    std::vector<float> buffer_;  // allocated once at construction
    const size_t capacity_;
    const size_t bufferSize_;    // next power of two >= capacity_
    const size_t mask_;

    std::atomic<size_t> head_{0};  // producer-owned
    std::atomic<size_t> tail_{0};  // consumer-owned

    static size_t nextPowerOfTwo(size_t v);
};

}  // namespace quadmapper
