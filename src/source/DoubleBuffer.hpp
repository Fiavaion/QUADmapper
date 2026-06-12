#pragma once

#include <mutex>
#include <atomic>

namespace quadmapper {

/**
 * @brief Thread-safe double buffer for producer-consumer pattern
 *
 * Used to transfer frame data between background threads (video decoder,
 * capture devices) and the main render thread. The producer writes to the
 * back buffer, then swaps. The consumer reads from the front buffer.
 *
 * @tparam T The data type to double-buffer (typically FrameData)
 */
template<typename T>
class DoubleBuffer {
public:
    DoubleBuffer() = default;
    ~DoubleBuffer() = default;

    // Non-copyable
    DoubleBuffer(const DoubleBuffer&) = delete;
    DoubleBuffer& operator=(const DoubleBuffer&) = delete;

    /**
     * @brief Get the back buffer for writing (producer thread)
     * @return Reference to the back buffer
     *
     * Call this to get a buffer to write new data to.
     * After writing, call swap() to make it available to the consumer.
     */
    T& getBackBuffer() {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffers_[writeIndex_];
    }

    /**
     * @brief Swap buffers after writing new data (producer thread)
     *
     * After writing to the back buffer, call this to swap it to the front.
     * This atomically signals that new data is available.
     */
    void swap() {
        std::lock_guard<std::mutex> lock(mutex_);
        writeIndex_ = 1 - writeIndex_;
        hasNewData_.store(true, std::memory_order_release);
    }

    /**
     * @brief Check if new data is available (consumer thread)
     * @return true if the producer has swapped new data since last read
     */
    bool hasNewData() const {
        return hasNewData_.load(std::memory_order_acquire);
    }

    /**
     * @brief Get the front buffer for reading (consumer thread)
     * @return Const reference to the front buffer
     *
     * This also clears the "new data" flag.
     */
    const T& getFrontBuffer() {
        std::lock_guard<std::mutex> lock(mutex_);
        hasNewData_.store(false, std::memory_order_release);
        return buffers_[1 - writeIndex_];
    }

    /**
     * @brief Get the front buffer without clearing the new data flag
     * @return Const reference to the front buffer
     */
    const T& peekFrontBuffer() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffers_[1 - writeIndex_];
    }

    /**
     * @brief Clear both buffers
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        buffers_[0] = T{};
        buffers_[1] = T{};
        hasNewData_.store(false, std::memory_order_release);
    }

private:
    T buffers_[2];
    int writeIndex_ = 0;
    mutable std::mutex mutex_;
    std::atomic<bool> hasNewData_{false};
};

} // namespace quadmapper
