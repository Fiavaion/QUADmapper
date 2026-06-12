#include "AudioBuffer.hpp"
#include <chrono>
#include <algorithm>
#include <cstring>

namespace quadmapper {

AudioBuffer::AudioBuffer(size_t capacity)
    : capacity_(capacity)
{
    buffer_.resize(capacity_);
}

AudioBuffer::~AudioBuffer() {
    clear();
}

bool AudioBuffer::push(const float* samples, size_t count, double pts) {
    if (!samples || count == 0) return true;

    std::lock_guard<std::mutex> lock(mutex_);

    size_t freeSpace = capacity_ - count_;
    if (count > freeSpace) {
        return false;  // Not enough space
    }

    // Update PTS tracking if buffer was empty
    if (count_ == 0) {
        currentPTS_ = pts;
    }

    // Copy samples into ring buffer
    size_t firstPart = std::min(count, capacity_ - head_);
    std::memcpy(buffer_.data() + head_, samples, firstPart * sizeof(float));

    if (count > firstPart) {
        // Wrap around
        std::memcpy(buffer_.data(), samples + firstPart, (count - firstPart) * sizeof(float));
    }

    head_ = (head_ + count) % capacity_;
    count_ += count;

    notEmpty_.notify_one();
    return true;
}

bool AudioBuffer::pushBlocking(const float* samples, size_t count, double pts, uint32_t timeoutMs) {
    if (!samples || count == 0) return true;

    std::unique_lock<std::mutex> lock(mutex_);

    auto predicate = [this, count]() {
        return (capacity_ - count_) >= count || endOfStream_;
    };

    if (timeoutMs > 0) {
        if (!notFull_.wait_for(lock, std::chrono::milliseconds(timeoutMs), predicate)) {
            return false;  // Timeout
        }
    } else {
        notFull_.wait(lock, predicate);
    }

    if (endOfStream_) {
        return false;
    }

    // Update PTS tracking if buffer was empty
    if (count_ == 0) {
        currentPTS_ = pts;
    }

    // Copy samples into ring buffer
    size_t firstPart = std::min(count, capacity_ - head_);
    std::memcpy(buffer_.data() + head_, samples, firstPart * sizeof(float));

    if (count > firstPart) {
        // Wrap around
        std::memcpy(buffer_.data(), samples + firstPart, (count - firstPart) * sizeof(float));
    }

    head_ = (head_ + count) % capacity_;
    count_ += count;

    notEmpty_.notify_one();
    return true;
}

size_t AudioBuffer::read(float* output, size_t sampleCount, int channels) {
    if (!output || sampleCount == 0) return 0;

    std::lock_guard<std::mutex> lock(mutex_);

    size_t toRead = std::min(sampleCount, count_);
    if (toRead == 0) {
        return 0;
    }

    // Copy samples from ring buffer
    size_t firstPart = std::min(toRead, capacity_ - tail_);
    std::memcpy(output, buffer_.data() + tail_, firstPart * sizeof(float));

    if (toRead > firstPart) {
        // Wrap around
        std::memcpy(output + firstPart, buffer_.data(), (toRead - firstPart) * sizeof(float));
    }

    tail_ = (tail_ + toRead) % capacity_;
    count_ -= toRead;

    // Update PTS based on samples read
    if (channels > 0 && ptsIncrement_ > 0) {
        currentPTS_ += (static_cast<double>(toRead) / channels) * ptsIncrement_;
    }

    notFull_.notify_one();
    return toRead;
}

size_t AudioBuffer::readBlocking(float* output, size_t sampleCount, int channels, uint32_t timeoutMs) {
    if (!output || sampleCount == 0) return 0;

    std::unique_lock<std::mutex> lock(mutex_);

    auto predicate = [this]() {
        return count_ > 0 || endOfStream_;
    };

    if (timeoutMs > 0) {
        if (!notEmpty_.wait_for(lock, std::chrono::milliseconds(timeoutMs), predicate)) {
            return 0;  // Timeout
        }
    } else {
        notEmpty_.wait(lock, predicate);
    }

    size_t toRead = std::min(sampleCount, count_);
    if (toRead == 0) {
        return 0;  // End of stream with empty buffer
    }

    // Copy samples from ring buffer
    size_t firstPart = std::min(toRead, capacity_ - tail_);
    std::memcpy(output, buffer_.data() + tail_, firstPart * sizeof(float));

    if (toRead > firstPart) {
        // Wrap around
        std::memcpy(output + firstPart, buffer_.data(), (toRead - firstPart) * sizeof(float));
    }

    tail_ = (tail_ + toRead) % capacity_;
    count_ -= toRead;

    // Update PTS based on samples read
    if (channels > 0 && ptsIncrement_ > 0) {
        currentPTS_ += (static_cast<double>(toRead) / channels) * ptsIncrement_;
    }

    notFull_.notify_one();
    return toRead;
}

void AudioBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    head_ = 0;
    tail_ = 0;
    count_ = 0;
    currentPTS_ = 0.0;

    notFull_.notify_all();
}

bool AudioBuffer::isEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ == 0;
}

bool AudioBuffer::isFull() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ >= capacity_;
}

size_t AudioBuffer::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

size_t AudioBuffer::capacity() const {
    return capacity_;
}

double AudioBuffer::getCurrentPTS() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) {
        return -1.0;
    }
    return currentPTS_;
}

void AudioBuffer::signalEndOfStream() {
    endOfStream_ = true;
    notEmpty_.notify_all();
    notFull_.notify_all();
}

bool AudioBuffer::isEndOfStream() const {
    return endOfStream_;
}

void AudioBuffer::resetEndOfStream() {
    endOfStream_ = false;
}

} // namespace quadmapper
