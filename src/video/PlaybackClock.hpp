#pragma once

#include "core/Types.hpp"

#include <chrono>
#include <cstdint>

namespace quadmapper {

// Playback timing (ADR-050 §1). Media time follows the wall clock by construction:
// the clock stores an anchor and computes every position from it, so no rounding
// accumulates, and Loop / Ping-Pong wrap analytically instead of restarting.
//
// Two layers:
//   timeline  unwrapped media seconds: anchor + (now - anchorWall) x speed x direction
//             while running. Only play, pause, seek, speed, direction and follow move
//             the anchor.
//   clip      the timeline mapped onto one clip (mapTimeline): clamped for Once,
//             modulo the duration for Loop, a reflected triangle wave for Ping-Pong.
//
// Nothing here reads a clock: every call takes `now`, so tests inject time.

using PlaybackTimePoint = std::chrono::steady_clock::time_point;

// Durations shorter than this count as unknown: the timeline is not wrapped.
constexpr double MIN_WRAP_DURATION = 1e-3;

// Where a timeline instant falls in the clip.
struct ClipPosition {
    double position = 0.0;  // seconds into the clip, 0..duration
    int direction = 1;      // +1 forward, -1 backward (Ping-Pong reverse leg, reversed clock)
    int64_t leg = 0;        // index of the pass through the clip (always 0 for Once)
    bool ended = false;     // Once: the timeline has reached the end of the clip
};

ClipPosition mapTimeline(double timeline, double duration, LoopMode mode);

// One pass through the clip in one direction: a Loop iteration or a Ping-Pong leg.
// Decoders walk legs to produce frames in timeline order.
struct PlaybackLeg {
    int64_t index = 0;
    double start = 0.0;     // timeline time at which the pass starts
    int direction = 1;      // +1 plays 0 -> duration, -1 plays duration -> 0
    bool final = true;      // no leg follows (Once, or an unknown duration)
};

PlaybackLeg legAt(double timeline, double duration, LoopMode mode);
PlaybackLeg nextLeg(const PlaybackLeg& leg, double duration, LoopMode mode);

// Timeline time at which a frame showing clip interval [framePts, frameEnd) becomes
// current during `leg`. Forward legs reach framePts first; reverse legs reach frameEnd.
double frameDueTime(const PlaybackLeg& leg, double duration, double framePts, double frameEnd);

class PlaybackClock {
public:
    // --- Queries ---

    // Unwrapped media seconds at `now`.
    double timeline(PlaybackTimePoint now) const;
    // The timeline mapped onto the clip; direction includes the clock's own direction.
    ClipPosition sample(PlaybackTimePoint now) const;
    double position(PlaybackTimePoint now) const { return sample(now).position; }
    int direction(PlaybackTimePoint now) const { return sample(now).direction; }

    // Counts every break in continuous playback: seeks, Loop wraps, Ping-Pong
    // reflections, a mode or duration change that moves the clip position, and a
    // followed master's restart. Never decreases. A consumer that remembers the last
    // value it saw knows it must resynchronise when the value changes.
    uint64_t discontinuity(PlaybackTimePoint now) const;

    bool running() const { return running_; }
    double speed() const { return speed_; }
    int baseDirection() const { return direction_; }  // set by setDirection
    double duration() const { return duration_; }
    LoopMode loopMode() const { return loopMode_; }

    // --- Transport; each re-anchors at `now`, so positions stay exact ---

    void play(PlaybackTimePoint now);
    void pause(PlaybackTimePoint now);
    // Jump to a clip position. On a Ping-Pong reverse leg playback keeps reversing.
    void seek(double position, PlaybackTimePoint now);
    // Speed must be positive; other values are ignored.
    void setSpeed(double speed, PlaybackTimePoint now);
    // +1 plays forward, -1 backward; other values are ignored.
    void setDirection(int direction, PlaybackTimePoint now);
    // Changing the mapping keeps the clip position, and the direction where the new
    // mode has one (Once and Loop only play forward).
    void setDuration(double duration, PlaybackTimePoint now);
    void setLoopMode(LoopMode mode, PlaybackTimePoint now);

    // --- Timeline lock (ADR-050 §3) ---

    // Take the master's timeline (anchor, speed, direction, running) and keep this
    // clock's own duration and loop mode, so position = master time mapped onto this
    // clip. Call whenever the master may have changed; returns true when this clock
    // jumped (the first follow, or the master was seeked) rather than continuing.
    bool follow(const PlaybackClock& master, PlaybackTimePoint now);
    // Continue on this clock's own timeline from where following left it.
    void stopFollowing() { following_ = false; }
    bool following() const { return following_; }

private:
    // Move the anchor to `now` without changing the timeline, folding the legs
    // crossed since the last anchor into the discontinuity count.
    void reanchor(PlaybackTimePoint now);
    // Jump the timeline, counting one discontinuity.
    void jumpTo(double timeline, PlaybackTimePoint now);
    int64_t legIndex(double timeline) const;

    double anchorTimeline_ = 0.0;
    PlaybackTimePoint anchorWall_{};
    double speed_ = 1.0;
    int direction_ = 1;
    bool running_ = false;

    double duration_ = 0.0;
    LoopMode loopMode_ = LoopMode::ONCE;

    uint64_t jumps_ = 0;       // discontinuities counted up to the anchor
    int64_t anchorLeg_ = 0;    // leg at the anchor
    bool following_ = false;
    uint64_t masterJumps_ = 0; // master's jump count when last followed
};

}  // namespace quadmapper
