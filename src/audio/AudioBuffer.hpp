#pragma once

#include <vector>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <cstdint>

namespace quadmapper {

/**
 * @brief Thread-safe ring buffer for audio samples
 *
 * Used for buffering decoded audio samples between the decoder thread
 * and the audio output thread. Stores contiguous interleaved float samples.
 */
class AudioBuffer {
public:
    /**
     * @brief Construct an audio buffer
     * @param capacity Maximum number of samples to buffer (total across all channels)
     */
    explicit AudioBuffer(size_t capacity = 48000 * 2 * 2);  // 2 seconds stereo default
    ~AudioBuffer();

    // Non-copyable
    AudioBuffer(const AudioBuffer&) = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;

    /**
     * @brief Push samples into the buffer
     * @param samples Pointer to interleaved float samples
     * @param count Number of samples to push (total, not per channel)
     * @param pts Presentation timestamp of first sample in seconds
     * @return true if all samples were pushed, false if buffer is full
     */
    bool push(const float* samples, size_t count, double pts);

    /**
     * @brief Push samples, blocking until space is available
     * @param samples Pointer to interleaved float samples
     * @param count Number of samples to push
     * @param pts Presentation timestamp of first sample
     * @param timeoutMs Timeout in milliseconds (0 = infinite)
     * @return true if successful, false if timeout or end of stream
     */
    bool pushBlocking(const float* samples, size_t count, double pts, uint32_t timeoutMs = 0);

    /**
     * @brief Read samples from the buffer
     * @param output Buffer to write samples to
     * @param sampleCount Number of samples to read (total, not per channel)
     * @param channels Number of channels (for calculating frame count)
     * @return Number of samples actually read
     */
    size_t read(float* output, size_t sampleCount, int channels);

    /**
     * @brief Read samples, blocking until enough are available
     * @param output Buffer to write samples to
     * @param sampleCount Number of samples to read
     * @param channels Number of channels
     * @param timeoutMs Timeout in milliseconds (0 = infinite)
     * @return Number of samples actually read
     */
    size_t readBlocking(float* output, size_t sampleCount, int channels, uint32_t timeoutMs = 0);

    /**
     * @brief Clear all samples from the buffer
     */
    void clear();

    /**
     * @brief Check if buffer is empty
     */
    bool isEmpty() const;

    /**
     * @brief Check if buffer is full
     */
    bool isFull() const;

    /**
     * @brief Get number of samples currently in buffer
     */
    size_t available() const;

    /**
     * @brief Get buffer capacity in samples
     */
    size_t capacity() const;

    /**
     * @brief Get the presentation timestamp of the next sample to be read
     * @return PTS in seconds, or -1 if buffer is empty
     */
    double getCurrentPTS() const;

    /**
     * @brief Signal that no more samples will be pushed (EOF)
     */
    void signalEndOfStream();

    /**
     * @brief Check if end of stream has been signaled
     */
    bool isEndOfStream() const;

    /**
     * @brief Reset end of stream flag (for looping)
     */
    void resetEndOfStream();

private:
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;

    std::vector<float> buffer_;
    size_t capacity_;
    size_t head_ = 0;           // Write position
    size_t tail_ = 0;           // Read position
    size_t count_ = 0;          // Current sample count

    double currentPTS_ = 0.0;   // PTS of next sample to be read
    double ptsIncrement_ = 0.0; // PTS increment per sample (1/sampleRate)
    int channels_ = 2;          // Channel count for PTS calculation

    std::atomic<bool> endOfStream_{false};
};

} // namespace quadmapper
