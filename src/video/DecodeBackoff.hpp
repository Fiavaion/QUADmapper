#pragma once

#include <algorithm>
#include <chrono>

namespace quadmapper {

// Consecutive-failure counter for a decode thread (QM-088). A few failures in a row
// are retried at once (a damaged packet); past the threshold the thread is in an
// error state and waits between attempts, doubling up to a cap, instead of spinning
// a core at 100%. Any success resets it.
class DecodeBackoff {
public:
    static constexpr int ERROR_THRESHOLD = 5;
    static constexpr std::chrono::milliseconds FIRST_DELAY{10};
    static constexpr std::chrono::milliseconds MAX_DELAY{1000};

    // Record one failure; returns how long to wait before the next attempt.
    std::chrono::milliseconds onFailure() {
        ++failures_;
        if (failures_ < ERROR_THRESHOLD) return std::chrono::milliseconds{0};
        const int doublings = std::min(failures_ - ERROR_THRESHOLD, 16);
        return std::min(FIRST_DELAY * (1LL << doublings), MAX_DELAY);
    }

    void onSuccess() { failures_ = 0; }

    // True once the failures have reached the threshold (surface it to the user).
    bool failing() const { return failures_ >= ERROR_THRESHOLD; }

private:
    int failures_ = 0;
};

}  // namespace quadmapper
