#include "PlaybackClock.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace quadmapper {

namespace {

bool durationKnown(double duration) {
    return std::isfinite(duration) && duration >= MIN_WRAP_DURATION;
}

bool wraps(double duration, LoopMode mode) {
    return durationKnown(duration) && mode != LoopMode::ONCE;
}

// Split a timeline instant into (pass index, offset into that pass), the offset kept
// inside [0, duration] despite rounding in the division.
void splitTimeline(double timeline, double duration, int64_t& index, double& offset) {
    index = static_cast<int64_t>(std::floor(timeline / duration));
    offset = std::clamp(timeline - static_cast<double>(index) * duration, 0.0, duration);
}

bool isReverseLeg(int64_t index) {
    return index % 2 != 0;  // also true for negative odd indices
}

// A timeline instant that maps to `position` travelling in `direction`.
double timelineFor(double position, int direction, double duration, LoopMode mode) {
    if (mode == LoopMode::PING_PONG && durationKnown(duration) && direction < 0) {
        return 2.0 * duration - position;  // the first reverse leg
    }
    return position;
}

}  // namespace

ClipPosition mapTimeline(double timeline, double duration, LoopMode mode) {
    ClipPosition clip;
    if (!durationKnown(duration)) {
        clip.position = std::max(timeline, 0.0);
        return clip;
    }
    if (mode == LoopMode::ONCE) {
        clip.position = std::clamp(timeline, 0.0, duration);
        clip.ended = timeline >= duration;
        return clip;
    }
    double offset = 0.0;
    splitTimeline(timeline, duration, clip.leg, offset);
    if (mode == LoopMode::PING_PONG && isReverseLeg(clip.leg)) {
        clip.position = duration - offset;
        clip.direction = -1;
    } else {
        clip.position = offset;
    }
    return clip;
}

PlaybackLeg legAt(double timeline, double duration, LoopMode mode) {
    PlaybackLeg leg;
    if (!wraps(duration, mode)) return leg;  // one forward pass from timeline 0
    double offset = 0.0;
    splitTimeline(timeline, duration, leg.index, offset);
    leg.start = static_cast<double>(leg.index) * duration;
    leg.direction = mode == LoopMode::PING_PONG && isReverseLeg(leg.index) ? -1 : 1;
    leg.final = false;
    return leg;
}

PlaybackLeg nextLeg(const PlaybackLeg& leg, double duration, LoopMode mode) {
    if (leg.final || !wraps(duration, mode)) {
        PlaybackLeg last = leg;
        last.final = true;
        return last;
    }
    PlaybackLeg next;
    next.index = leg.index + 1;
    next.start = static_cast<double>(next.index) * duration;
    next.direction = mode == LoopMode::PING_PONG && isReverseLeg(next.index) ? -1 : 1;
    next.final = false;
    return next;
}

double frameDueTime(const PlaybackLeg& leg, double duration, double framePts, double frameEnd) {
    return leg.direction > 0 ? leg.start + framePts : leg.start + duration - frameEnd;
}

// --- PlaybackClock ---

double PlaybackClock::timeline(PlaybackTimePoint now) const {
    if (!running_) return anchorTimeline_;
    const double elapsed = std::chrono::duration<double>(now - anchorWall_).count();
    return anchorTimeline_ + elapsed * speed_ * static_cast<double>(direction_);
}

ClipPosition PlaybackClock::sample(PlaybackTimePoint now) const {
    ClipPosition clip = mapTimeline(timeline(now), duration_, loopMode_);
    clip.direction *= direction_;
    return clip;
}

int64_t PlaybackClock::legIndex(double t) const {
    return mapTimeline(t, duration_, loopMode_).leg;
}

uint64_t PlaybackClock::discontinuity(PlaybackTimePoint now) const {
    return jumps_ + static_cast<uint64_t>(std::llabs(legIndex(timeline(now)) - anchorLeg_));
}

void PlaybackClock::reanchor(PlaybackTimePoint now) {
    anchorTimeline_ = timeline(now);
    anchorWall_ = now;
    const int64_t leg = legIndex(anchorTimeline_);
    jumps_ += static_cast<uint64_t>(std::llabs(leg - anchorLeg_));
    anchorLeg_ = leg;
}

void PlaybackClock::jumpTo(double t, PlaybackTimePoint now) {
    reanchor(now);
    anchorTimeline_ = t;
    anchorLeg_ = legIndex(t);
    ++jumps_;
}

void PlaybackClock::play(PlaybackTimePoint now) {
    reanchor(now);
    running_ = true;
}

void PlaybackClock::pause(PlaybackTimePoint now) {
    reanchor(now);
    running_ = false;
}

void PlaybackClock::seek(double position, PlaybackTimePoint now) {
    if (durationKnown(duration_)) position = std::clamp(position, 0.0, duration_);
    position = std::max(position, 0.0);
    const int legDirection = mapTimeline(timeline(now), duration_, loopMode_).direction;
    jumpTo(timelineFor(position, legDirection, duration_, loopMode_), now);
}

void PlaybackClock::setSpeed(double speed, PlaybackTimePoint now) {
    if (!(speed > 0.0)) return;
    reanchor(now);
    speed_ = speed;
}

void PlaybackClock::setDirection(int direction, PlaybackTimePoint now) {
    if (direction != 1 && direction != -1) return;
    reanchor(now);
    direction_ = direction;
}

void PlaybackClock::setDuration(double duration, PlaybackTimePoint now) {
    if (!(duration >= 0.0)) duration = 0.0;
    const ClipPosition before = mapTimeline(timeline(now), duration_, loopMode_);
    reanchor(now);
    duration_ = duration;
    if (following_) {
        // The timeline is the master's: only the mapping changes.
        anchorLeg_ = legIndex(anchorTimeline_);
        if (mapTimeline(anchorTimeline_, duration_, loopMode_).position != before.position) ++jumps_;
        return;
    }
    const double position = durationKnown(duration_) ? std::min(before.position, duration_)
                                                    : before.position;
    anchorTimeline_ = timelineFor(position, before.direction, duration_, loopMode_);
    anchorLeg_ = legIndex(anchorTimeline_);
    if (position != before.position) ++jumps_;
}

void PlaybackClock::setLoopMode(LoopMode mode, PlaybackTimePoint now) {
    const ClipPosition before = mapTimeline(timeline(now), duration_, loopMode_);
    reanchor(now);
    loopMode_ = mode;
    if (following_) {
        anchorLeg_ = legIndex(anchorTimeline_);
        if (mapTimeline(anchorTimeline_, duration_, loopMode_).position != before.position) ++jumps_;
        return;
    }
    anchorTimeline_ = timelineFor(before.position, before.direction, duration_, loopMode_);
    anchorLeg_ = legIndex(anchorTimeline_);
}

bool PlaybackClock::follow(const PlaybackClock& master, PlaybackTimePoint now) {
    const bool jumped = !following_ || master.jumps_ != masterJumps_;
    if (!jumped && master.anchorTimeline_ == anchorTimeline_ && master.anchorWall_ == anchorWall_ &&
        master.speed_ == speed_ && master.direction_ == direction_ && master.running_ == running_) {
        return false;  // nothing changed since the last follow
    }
    // Count the legs crossed on the old anchor, then switch to the master's.
    const int64_t legBefore = legIndex(timeline(now));
    jumps_ += static_cast<uint64_t>(std::llabs(legBefore - anchorLeg_));
    anchorTimeline_ = master.anchorTimeline_;
    anchorWall_ = master.anchorWall_;
    speed_ = master.speed_;
    direction_ = master.direction_;
    running_ = master.running_;
    anchorLeg_ = legIndex(timeline(now));
    if (jumped) {
        ++jumps_;
    } else {
        // A pause lands a hair behind the extrapolated time; crossing a leg boundary
        // backwards that way still counts, so the count never goes down.
        jumps_ += static_cast<uint64_t>(std::llabs(anchorLeg_ - legBefore));
    }
    following_ = true;
    masterJumps_ = master.jumps_;
    return jumped;
}

}  // namespace quadmapper
