#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace quadmapper {

// A single-writer value that any thread can read without a lock (a seqlock). The
// writer never waits, so the real-time audio callback may write one; a reader that
// overlaps a write gets `false` and keeps its last good copy (or retries, off the
// real-time thread). The value lives in atomic words, so there is no data race.
template <typename T>
class SeqLockValue {
    static_assert(std::is_trivially_copyable<T>::value, "SeqLockValue needs a trivially copyable T");

public:
    SeqLockValue() { store(T{}); }

    // Writer thread only.
    void store(const T& value) {
        uint64_t words[WORDS] = {};
        std::memcpy(words, &value, sizeof(T));
        const uint32_t seq = sequence_.load(std::memory_order_relaxed);
        sequence_.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < WORDS; ++i) words_[i].store(words[i], std::memory_order_relaxed);
        sequence_.store(seq + 2, std::memory_order_release);
    }

    // Any thread. Returns false, leaving `out` untouched, when a write overlapped.
    bool tryLoad(T& out) const {
        const uint32_t before = sequence_.load(std::memory_order_acquire);
        if (before & 1u) return false;
        uint64_t words[WORDS];
        for (size_t i = 0; i < WORDS; ++i) words[i] = words_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sequence_.load(std::memory_order_relaxed) != before) return false;
        std::memcpy(&out, words, sizeof(T));
        return true;
    }

    // Off the real-time thread: retry until a consistent copy is read.
    T load() const {
        T value{};
        while (!tryLoad(value)) {
        }
        return value;
    }

private:
    static constexpr size_t WORDS = (sizeof(T) + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    std::atomic<uint32_t> sequence_{0};
    std::atomic<uint64_t> words_[WORDS];
};

}  // namespace quadmapper
