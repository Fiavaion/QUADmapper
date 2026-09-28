#include "ControlEventQueue.hpp"

#include <algorithm>

namespace quadmapper::control {

ControlEventQueue::ControlEventQueue(size_t capacity)
    : slots_(std::max<size_t>(1, capacity)) {}

bool ControlEventQueue::push(ControlEvent&& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == slots_.size()) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    slots_[(head_ + count_) % slots_.size()] = std::move(event);
    ++count_;
    return true;
}

size_t ControlEventQueue::drain(std::vector<ControlEvent>& out, size_t maxEvents) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t n = std::min(count_, maxEvents);
    for (size_t k = 0; k < n; ++k) {
        out.push_back(std::move(slots_[head_]));
        head_ = (head_ + 1) % slots_.size();
    }
    count_ -= n;
    return n;
}

size_t ControlEventQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

}  // namespace quadmapper::control
