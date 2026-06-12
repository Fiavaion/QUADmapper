#include "FrameBuffer.hpp"
#include <chrono>

namespace quadmapper {

FrameBuffer::FrameBuffer(size_t capacity)
    : capacity_(capacity)
{
    buffer_.resize(capacity_);
}

FrameBuffer::~FrameBuffer() {
    clear();
}

bool FrameBuffer::push(VideoFrame&& frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (count_ >= capacity_) {
        return false;  // Buffer full
    }

    buffer_[head_] = std::move(frame);
    head_ = (head_ + 1) % capacity_;
    ++count_;

    notEmpty_.notify_one();
    return true;
}

bool FrameBuffer::pushBlocking(VideoFrame&& frame, uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);

    auto predicate = [this]() { return count_ < capacity_ || endOfStream_; };

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

    buffer_[head_] = std::move(frame);
    head_ = (head_ + 1) % capacity_;
    ++count_;

    notEmpty_.notify_one();
    return true;
}

std::optional<VideoFrame> FrameBuffer::pop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (count_ == 0) {
        return std::nullopt;
    }

    VideoFrame frame = std::move(buffer_[tail_]);
    buffer_[tail_] = VideoFrame{};  // Clear moved-from frame
    tail_ = (tail_ + 1) % capacity_;
    --count_;

    notFull_.notify_one();
    return frame;
}

std::optional<VideoFrame> FrameBuffer::popBlocking(uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);

    auto predicate = [this]() { return count_ > 0 || endOfStream_; };

    if (timeoutMs > 0) {
        if (!notEmpty_.wait_for(lock, std::chrono::milliseconds(timeoutMs), predicate)) {
            return std::nullopt;  // Timeout
        }
    } else {
        notEmpty_.wait(lock, predicate);
    }

    if (count_ == 0) {
        return std::nullopt;  // End of stream with empty buffer
    }

    VideoFrame frame = std::move(buffer_[tail_]);
    buffer_[tail_] = VideoFrame{};
    tail_ = (tail_ + 1) % capacity_;
    --count_;

    notFull_.notify_one();
    return frame;
}

const VideoFrame* FrameBuffer::peek() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (count_ == 0) {
        return nullptr;
    }

    return &buffer_[tail_];
}

double FrameBuffer::peekPTS() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (count_ == 0) {
        return -1.0;
    }

    return buffer_[tail_].pts;
}

void FrameBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (size_t i = 0; i < capacity_; ++i) {
        buffer_[i] = VideoFrame{};
    }

    head_ = 0;
    tail_ = 0;
    count_ = 0;

    notFull_.notify_all();
}

bool FrameBuffer::isEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ == 0;
}

bool FrameBuffer::isFull() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ >= capacity_;
}

size_t FrameBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

size_t FrameBuffer::capacity() const {
    return capacity_;
}

void FrameBuffer::signalEndOfStream() {
    endOfStream_ = true;
    notEmpty_.notify_all();
    notFull_.notify_all();
}

bool FrameBuffer::isEndOfStream() const {
    return endOfStream_;
}

void FrameBuffer::resetEndOfStream() {
    endOfStream_ = false;
}

} // namespace quadmapper
