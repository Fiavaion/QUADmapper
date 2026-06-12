#pragma once

#include "VideoDecoder.hpp"
#include <vector>
#include <mutex>
#include <atomic>
#include <optional>

namespace quadmapper {

/**
 * @brief Thread-safe ring buffer for video frames
 *
 * Used for staging decoded frames before GPU upload.
 * Supports multiple producers (decoder threads) and single consumer (render thread).
 */
class FrameBuffer {
public:
    /**
     * @brief Construct a frame buffer
     * @param capacity Maximum number of frames to buffer
     */
    explicit FrameBuffer(size_t capacity = 8);
    ~FrameBuffer();

    // Non-copyable
    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;

    /**
     * @brief Push a frame into the buffer
     * @param frame Frame to push
     * @return true if successful, false if buffer is full
     */
    bool push(VideoFrame&& frame);

    /**
     * @brief Push a frame, blocking until space is available
     * @param frame Frame to push
     * @param timeoutMs Timeout in milliseconds (0 = infinite)
     * @return true if successful, false if timeout
     */
    bool pushBlocking(VideoFrame&& frame, uint32_t timeoutMs = 0);

    /**
     * @brief Pop a frame from the buffer
     * @return Frame if available, nullopt otherwise
     */
    std::optional<VideoFrame> pop();

    /**
     * @brief Pop a frame, blocking until one is available
     * @param timeoutMs Timeout in milliseconds (0 = infinite)
     * @return Frame if available, nullopt if timeout
     */
    std::optional<VideoFrame> popBlocking(uint32_t timeoutMs = 0);

    /**
     * @brief Peek at the next frame without removing it
     * @return Pointer to frame if available, nullptr otherwise
     */
    const VideoFrame* peek() const;

    /**
     * @brief Get the timestamp of the next frame
     * @return PTS of next frame, or -1 if buffer empty
     */
    double peekPTS() const;

    /**
     * @brief Clear all frames from the buffer
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
     * @brief Get current number of frames in buffer
     */
    size_t size() const;

    /**
     * @brief Get buffer capacity
     */
    size_t capacity() const;

    /**
     * @brief Signal that no more frames will be pushed (EOF)
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

    std::vector<VideoFrame> buffer_;
    size_t capacity_;
    size_t head_ = 0;       // Write position
    size_t tail_ = 0;       // Read position
    size_t count_ = 0;      // Current frame count

    std::atomic<bool> endOfStream_{false};
};

} // namespace quadmapper
