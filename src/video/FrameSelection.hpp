#pragma once

#include <cstddef>

namespace quadmapper {

// Which decoded frames a player shows on an update (QM-088). Shared by VideoPlayer
// and HapFileSource so both pace playback by the clock, not by the render loop.

// Slack for floating-point clock accumulation: a frame whose pts equals the target
// must not slip a whole update because the summed deltas land a hair below it.
constexpr double FRAME_DUE_EPSILON = 1e-6;

// A frame is due once the playback clock reaches its pts. With nothing on screen
// yet, the first frame is due at once, so a clip whose first pts is slightly
// positive does not start black.
inline bool isFrameDue(double framePts, double targetPts, bool haveFrame) {
    return !haveFrame || framePts <= targetPts + FRAME_DUE_EPSILON;
}

// Pop every due frame off the front of a pts-ordered queue. The last one popped is
// the frame to show; earlier due frames were late and are dropped. Frames ahead of
// the target stay queued, so playback never runs ahead of the clock.
//   frontPts(double& pts) -> bool : peek the front frame's pts; false when empty
//   popFront()                   : take the front frame as the displayed frame
// Returns how many frames were popped (0 = keep showing the current frame).
template <typename FrontPts, typename PopFront>
size_t popDueFrames(double targetPts, bool haveFrame, FrontPts&& frontPts, PopFront&& popFront) {
    size_t popped = 0;
    double pts = 0.0;
    while (frontPts(pts) && isFrameDue(pts, targetPts, haveFrame || popped > 0)) {
        popFront();
        ++popped;
    }
    return popped;
}

}  // namespace quadmapper
