#pragma once

#include <cstddef>

namespace quadmapper {

// Which decoded frames a player shows on an update (QM-088). Shared by VideoPlayer
// and HapFileSource so both pace playback by the clock, not by the render loop.
// Frames carry the playback-clock timeline time at which they become current
// (ADR-050), so the same rule serves forward, looped and reversed playback.

// Slack for floating-point rounding: a frame due exactly at the target must not
// slip a whole update because the clock lands a hair below it.
constexpr double FRAME_DUE_EPSILON = 1e-6;

// A player whose decoder has fallen this far behind the clock (a stall, or master
// time moving on while a locked source was hidden) restarts decoding at the clock
// rather than decoding everything in between.
constexpr double RESYNC_LATENESS = 1.0;

// A frame is due once the playback clock reaches its due time. With nothing on
// screen yet, the first frame is due at once, so a clip whose first pts is slightly
// positive does not start black.
inline bool isFrameDue(double framePts, double targetPts, bool haveFrame) {
    return !haveFrame || framePts <= targetPts + FRAME_DUE_EPSILON;
}

// Pop every due frame off the front of a queue ordered by due time. The last one
// popped is the frame to show; earlier due frames were late and are dropped. Frames
// ahead of the target stay queued, so playback never runs ahead of the clock.
//   frontPts(double& pts) -> bool : peek the front frame's due time; false when empty
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
