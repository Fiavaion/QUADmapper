#pragma once

#include "ControlEvent.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace quadmapper::control {

// Mutex-guarded fixed-capacity ring of ControlEvents (ADR-048 §1). Producers are the
// OSC receive thread and WinMM MIDI callbacks; the main thread drains it once per
// frame. Slots are allocated once up front and events are moved in and out, so a
// push never allocates: a MIDI event holds no heap memory, and a drained slot is
// left moved-from (empty), so moving a MIDI event into it frees nothing either.
// When full, new events are dropped and counted; a flood never grows memory. OSC
// events are refused once the ring is three-quarters full, so MIDI always has room.
class ControlEventQueue {
public:
    static constexpr size_t kDefaultCapacity = 4096;

    explicit ControlEventQueue(size_t capacity = kDefaultCapacity);

    ControlEventQueue(const ControlEventQueue&) = delete;
    ControlEventQueue& operator=(const ControlEventQueue&) = delete;

    // False when the queue is full; the event is dropped and counted.
    bool push(ControlEvent&& event);

    // Move up to maxEvents of the oldest events onto the end of `out`. Returns how
    // many were moved.
    size_t drain(std::vector<ControlEvent>& out, size_t maxEvents);

    size_t size() const;
    size_t capacity() const { return slots_.size(); }
    uint64_t droppedCount() const { return dropped_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex mutex_;
    std::vector<ControlEvent> slots_;
    size_t head_ = 0;   // next slot to drain
    size_t count_ = 0;
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace quadmapper::control
