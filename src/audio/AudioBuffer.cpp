#include "AudioBuffer.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace quadmapper {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

size_t AudioBuffer::nextPowerOfTwo(size_t v) {
    if (v == 0) return 1;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AudioBuffer::AudioBuffer(size_t capacity)
    : capacity_(capacity)
    , bufferSize_(nextPowerOfTwo(capacity))
    , mask_(bufferSize_ - 1)
{
    buffer_.resize(bufferSize_);
}

AudioBuffer::~AudioBuffer() {
    // Signal any blocked threads so they can exit cleanly.
    endOfStream_ = true;
    notEmpty_.notify_all();
    notFull_.notify_all();
}

// ---------------------------------------------------------------------------
// Producer: push (non-blocking, lock-free)
// ---------------------------------------------------------------------------

bool AudioBuffer::push(const float* samples, size_t count, double pts) {
    if (!samples || count == 0) return true;

    // Producer reads tail with acquire to observe the consumer's latest read.
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t head = head_.load(std::memory_order_relaxed);
    // head and tail are monotonically increasing; unsigned subtraction is safe.
    const size_t used = head - tail;
    if (count > capacity_ - used) {
        return false;
    }

    // Record PTS when transitioning from empty to non-empty.
    if (used == 0) {
        currentPTS_.store(pts, std::memory_order_relaxed);
    }

    // Copy data in at most two parts (wrap-around case).
    const size_t writePos  = head & mask_;
    const size_t firstPart = std::min(count, bufferSize_ - writePos);
    std::memcpy(buffer_.data() + writePos, samples, firstPart * sizeof(float));

    if (count > firstPart) {
        std::memcpy(buffer_.data(), samples + firstPart,
                    (count - firstPart) * sizeof(float));
    }

    // Release store: makes the written data visible to the consumer.
    head_.store(head + count, std::memory_order_release);

    // Wake any blocked consumer.
    notEmpty_.notify_one();
    return true;
}

// ---------------------------------------------------------------------------
// Producer: push (blocking)
// ---------------------------------------------------------------------------

bool AudioBuffer::pushBlocking(const float* samples, size_t count, double pts,
                                uint32_t timeoutMs) {
    if (!samples || count == 0) return true;

    std::unique_lock<std::mutex> lock(blockingMutex_);

    auto hasSpace = [&]() {
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t head = head_.load(std::memory_order_relaxed);
        return (capacity_ - (head - tail)) >= count || endOfStream_.load(std::memory_order_relaxed);
    };

    if (timeoutMs > 0) {
        if (!notFull_.wait_for(lock, std::chrono::milliseconds(timeoutMs), hasSpace)) {
            return false;
        }
    } else {
        notFull_.wait(lock, hasSpace);
    }

    if (endOfStream_.load(std::memory_order_relaxed)) {
        return false;
    }

    lock.unlock();  // Release before the lock-free push so the RT thread isn't blocked.
    return push(samples, count, pts);
}

// ---------------------------------------------------------------------------
// Consumer: read (lock-free, RT-safe)
// ---------------------------------------------------------------------------

size_t AudioBuffer::read(float* output, size_t sampleCount, int /*channels*/) {
    if (!output || sampleCount == 0) return 0;

    // Consumer reads head with acquire to observe the producer's latest write.
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_relaxed);
    // head and tail are monotonically increasing; unsigned subtraction is safe.
    const size_t available = head - tail;

    const size_t toRead = std::min(sampleCount, available);
    if (toRead == 0) return 0;

    // Copy data in at most two parts.
    const size_t readPos   = tail & mask_;
    const size_t firstPart = std::min(toRead, bufferSize_ - readPos);
    std::memcpy(output, buffer_.data() + readPos, firstPart * sizeof(float));

    if (toRead > firstPart) {
        std::memcpy(output + firstPart, buffer_.data(),
                    (toRead - firstPart) * sizeof(float));
    }

    // Advance PTS by the number of frames consumed.
    // ptsIncrement_ is not stored anymore; callers that need PTS should use
    // getCurrentPTS() before calling read(). We update currentPTS_ to reflect
    // the head of what remains in the buffer. This is a best-effort estimate:
    // because we don't store per-sample PTS we cannot track it perfectly across
    // arbitrary read sizes, so we simply leave currentPTS_ pointing at the
    // start of the initial push block; users that need sub-frame PTS precision
    // should track it in their own layer.

    // Release store: makes the advanced tail visible to the producer.
    tail_.store(tail + toRead, std::memory_order_release);

    // Wake any blocked producer.
    notFull_.notify_one();
    return toRead;
}

// ---------------------------------------------------------------------------
// Consumer: read (blocking)
// ---------------------------------------------------------------------------

size_t AudioBuffer::readBlocking(float* output, size_t sampleCount, int channels,
                                  uint32_t timeoutMs) {
    if (!output || sampleCount == 0) return 0;

    std::unique_lock<std::mutex> lock(blockingMutex_);

    auto hasData = [&]() {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        return (head - tail) > 0 || endOfStream_.load(std::memory_order_relaxed);
    };

    if (timeoutMs > 0) {
        if (!notEmpty_.wait_for(lock, std::chrono::milliseconds(timeoutMs), hasData)) {
            return 0;
        }
    } else {
        notEmpty_.wait(lock, hasData);
    }

    lock.unlock();  // Release before the lock-free read.
    return read(output, sampleCount, channels);
}

// ---------------------------------------------------------------------------
// Control interface
// ---------------------------------------------------------------------------

void AudioBuffer::clear() {
    // Take the blocking mutex to synchronise with any blocked push/read callers.
    std::lock_guard<std::mutex> lock(blockingMutex_);

    // Reset both indices to zero. The consumer (audio thread) must not be
    // running concurrently when clear() is called; the caller is responsible
    // for stopping the audio device before clearing if it is running.
    tail_.store(0, std::memory_order_relaxed);
    head_.store(0, std::memory_order_relaxed);
    currentPTS_.store(0.0, std::memory_order_relaxed);

    notFull_.notify_all();
}

bool AudioBuffer::isEmpty() const {
    // head == tail means no samples are buffered.
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return head == tail;
}

bool AudioBuffer::isFull() const {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return (head - tail) >= capacity_;
}

size_t AudioBuffer::available() const {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return head - tail;
}

size_t AudioBuffer::capacity() const {
    return capacity_;
}

double AudioBuffer::getCurrentPTS() const {
    if (isEmpty()) return -1.0;
    return currentPTS_.load(std::memory_order_acquire);
}

void AudioBuffer::signalEndOfStream() {
    endOfStream_.store(true, std::memory_order_release);
    notEmpty_.notify_all();
    notFull_.notify_all();
}

bool AudioBuffer::isEndOfStream() const {
    return endOfStream_.load(std::memory_order_acquire);
}

void AudioBuffer::resetEndOfStream() {
    endOfStream_.store(false, std::memory_order_release);
}

} // namespace quadmapper
