#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace quadmapper {

/**
 * @brief Lock-free SPSC ring buffer for audio samples
 *
 * Threading model:
 *   Producer (decode / control thread): push(), pushBlocking(), clear(),
 *     signalEndOfStream(), resetEndOfStream().
 *   Consumer (audio RT callback): read(), isEmpty(), available(),
 *     getCurrentPTS().
 *
 * head_ is owned exclusively by the producer; tail_ is owned exclusively by
 * the consumer. Each side only stores into its own index and loads the
 * counterpart with acquire ordering, which gives correct visibility without
 * any mutex in the hot path.
 *
 * Blocking variants (pushBlocking / readBlocking) use a separate mutex only
 * to wait on condition variables; the actual index updates still go through
 * the atomic head_/tail_ path.
 *
 * capacity_ must be a power of two so wrap-around is a cheap bitmask.
 */
class AudioBuffer {
public:
    /**
     * @brief Construct an audio buffer
     * @param capacity Maximum number of samples (rounded up to next power of two)
     */
    explicit AudioBuffer(size_t capacity = 48000 * 2 * 2);  // 2 sec stereo default
    ~AudioBuffer();

    // Non-copyable
    AudioBuffer(const AudioBuffer&) = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;

    // --- Producer interface (decode thread) ---

    /**
     * @brief Push samples into the buffer (non-blocking).
     * @return true if all samples were pushed; false if not enough space.
     */
    bool push(const float* samples, size_t count, double pts);

    /**
     * @brief Push samples, blocking until space is available.
     * @param timeoutMs 0 = infinite wait.
     * @return true if successful; false on timeout or end-of-stream.
     */
    bool pushBlocking(const float* samples, size_t count, double pts,
                      uint32_t timeoutMs = 0);

    // --- Consumer interface (audio RT callback) ---

    /**
     * @brief Read samples from the buffer (lock-free, RT-safe).
     * @return Number of samples actually read.
     */
    size_t read(float* output, size_t sampleCount, int channels);

    /**
     * @brief Read samples, blocking until enough are available.
     * @param timeoutMs 0 = infinite wait.
     */
    size_t readBlocking(float* output, size_t sampleCount, int channels,
                        uint32_t timeoutMs = 0);

    // --- Control interface ---

    /**
     * @brief Clear all samples (producer must be stopped before calling).
     */
    void clear();

    /** Lock-free: true if no samples are available. RT-safe. */
    bool isEmpty() const;

    /** Lock-free: true if buffer is full. */
    bool isFull() const;

    /** Lock-free: samples currently in the buffer. RT-safe. */
    size_t available() const;

    size_t capacity() const;

    /**
     * @brief Presentation timestamp of the next sample to be read.
     * @return PTS in seconds, or -1 if buffer is empty.
     */
    double getCurrentPTS() const;

    /** Signal end-of-stream (no more pushes coming). */
    void signalEndOfStream();

    bool isEndOfStream() const;

    /** Reset EOS flag (for looping). */
    void resetEndOfStream();

private:
    // Ring buffer storage — allocated once at construction.
    std::vector<float> buffer_;
    const size_t capacity_;   // Always a power of two.
    const size_t mask_;       // capacity_ - 1; used for fast modulo.

    // head_ is the write position (producer-owned).
    // tail_ is the read  position (consumer-owned).
    // Available samples = (head_ - tail_) & mask_.
    // Both indices grow monotonically and wrap via masking.
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};

    // PTS of the next sample the consumer will read.
    // Written by the producer (when the buffer transitions empty→non-empty)
    // and read by the consumer. Relaxed store/load is sufficient because the
    // sequence point that makes the stored value meaningful (head_ update) uses
    // release/acquire ordering that already provides the necessary happens-before.
    std::atomic<double> currentPTS_{0.0};

    std::atomic<bool> endOfStream_{false};

    // Used only by the blocking variants to sleep on a condvar.
    // Never held by the RT audio callback.
    mutable std::mutex blockingMutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;

    static size_t nextPowerOfTwo(size_t v);
};

} // namespace quadmapper
