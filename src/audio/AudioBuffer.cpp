#include "AudioBuffer.hpp"

#include <algorithm>
#include <cstring>

namespace quadmapper {

size_t AudioBuffer::nextPowerOfTwo(size_t v) {
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

AudioBuffer::AudioBuffer(size_t capacity)
    : capacity_(capacity)
    , bufferSize_(nextPowerOfTwo(capacity))
    , mask_(bufferSize_ - 1) {
    buffer_.resize(bufferSize_);
}

bool AudioBuffer::push(const float* samples, size_t count) {
    if (!samples || count == 0) return true;

    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t head = head_.load(std::memory_order_relaxed);
    if (count > capacity_ - (head - tail)) return false;

    const size_t writePos = head & mask_;
    const size_t firstPart = std::min(count, bufferSize_ - writePos);
    std::memcpy(buffer_.data() + writePos, samples, firstPart * sizeof(float));
    if (count > firstPart) {
        std::memcpy(buffer_.data(), samples + firstPart, (count - firstPart) * sizeof(float));
    }
    // Release: the samples are visible before the consumer sees the new head.
    head_.store(head + count, std::memory_order_release);
    return true;
}

size_t AudioBuffer::read(float* output, size_t count) {
    if (!output || count == 0) return 0;

    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t toRead = std::min(count, head - tail);
    if (toRead == 0) return 0;

    const size_t readPos = tail & mask_;
    const size_t firstPart = std::min(toRead, bufferSize_ - readPos);
    std::memcpy(output, buffer_.data() + readPos, firstPart * sizeof(float));
    if (toRead > firstPart) {
        std::memcpy(output + firstPart, buffer_.data(), (toRead - firstPart) * sizeof(float));
    }
    // Release: the producer may reuse the space only after the copy is done.
    tail_.store(tail + toRead, std::memory_order_release);
    return toRead;
}

void AudioBuffer::discardTo(size_t position) {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t target = std::min(position, head);
    if (target > tail) tail_.store(target, std::memory_order_release);
}

size_t AudioBuffer::available() const {
    // Tail first: it never passes head, and head only grows, so head - tail cannot wrap
    // even when the other side moves between the two loads.
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t head = head_.load(std::memory_order_acquire);
    return head - tail;
}

}  // namespace quadmapper
