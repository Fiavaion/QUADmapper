#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace quadmapper {

/**
 * @brief Equal-power crossfade across a Loop wrap, on interleaved stereo samples
 *
 * A Loop pass runs straight into the next, so the sample before the wrap and the one
 * after it are unrelated and the step between them clicks. At a wrap the decode thread
 * arms this with the outgoing sound's continuation (what the file holds past the
 * clip's end, held at its last value where it runs out); the first frames() frames of
 * the next pass are then mixed as
 *     next * sin(t * pi/2) + continuation * cos(t * pi/2),   t = (i + 0.5) / frames()
 * No sample is added or removed, so the timeline (and sync) is untouched.
 *
 * Single-threaded (the decode thread). Every buffer is allocated in the constructor;
 * arming and mixing never allocate.
 */
class LoopCrossfade {
public:
    explicit LoopCrossfade(size_t frames)
        : frames_(std::max<size_t>(frames, 1))
        , fadeIn_(frames_)
        , fadeOut_(frames_)
        , continuation_(frames_ * 2, 0.0f)
        , scratch_(frames_ * 2, 0.0f) {
        const double quarter = std::acos(0.0);  // pi / 2
        for (size_t i = 0; i < frames_; ++i) {
            const double t = (static_cast<double>(i) + 0.5) / static_cast<double>(frames_);
            fadeIn_[i] = static_cast<float>(std::sin(t * quarter));
            fadeOut_[i] = static_cast<float>(std::cos(t * quarter));
        }
    }

    size_t frames() const { return frames_; }

    // Arm at a wrap. `tail` holds up to `tailFrames` frames of the outgoing sound past
    // the wrap; `last` is the last outgoing frame before it (used when there is no
    // tail). The fade starts after `delay` frames still to come from the outgoing pass
    // (its padding) and lasts at most `limit` frames (a pass shorter than the fade).
    void arm(const float* tail, size_t tailFrames, const float last[2], size_t delay, size_t limit) {
        tailFrames = std::min(tailFrames, frames_);
        std::copy(tail, tail + tailFrames * 2, continuation_.begin());
        float hold[2] = {last[0], last[1]};
        if (tailFrames > 0) {
            hold[0] = tail[(tailFrames - 1) * 2];
            hold[1] = tail[(tailFrames - 1) * 2 + 1];
        }
        for (size_t i = tailFrames; i < frames_; ++i) {
            continuation_[i * 2] = hold[0];
            continuation_[i * 2 + 1] = hold[1];
        }
        delay_ = delay;
        position_ = 0;
        length_ = std::min(frames_, std::max<size_t>(limit, 1));
    }

    void disarm() {
        delay_ = 0;
        position_ = length_ = 0;
    }

    // Outgoing frames still to pass through unchanged before the fade.
    size_t delay() const { return delay_; }
    // Frames of the fade still to mix (0: nothing armed).
    size_t remaining() const { return length_ - position_; }

    void skip(size_t count) { delay_ -= std::min(count, delay_); }

    // Mix the next `count` frames (count <= remaining(), delay() == 0) of the incoming
    // pass into the internal scratch; returns it.
    const float* mix(const float* incoming, size_t count) {
        count = std::min(count, remaining());
        for (size_t i = 0; i < count; ++i, ++position_) {
            // A fade cut short by a tiny pass jumps to its end at the last frame.
            const size_t g = position_ + 1 == length_ ? frames_ - 1 : position_ * frames_ / length_;
            for (int ch = 0; ch < 2; ++ch) {
                scratch_[i * 2 + ch] = incoming[i * 2 + ch] * fadeIn_[g] +
                                       continuation_[position_ * 2 + ch] * fadeOut_[g];
            }
        }
        return scratch_.data();
    }

private:
    const size_t frames_;
    std::vector<float> fadeIn_;
    std::vector<float> fadeOut_;
    std::vector<float> continuation_;
    std::vector<float> scratch_;
    size_t delay_ = 0;
    size_t position_ = 0;
    size_t length_ = 0;
};

}  // namespace quadmapper
