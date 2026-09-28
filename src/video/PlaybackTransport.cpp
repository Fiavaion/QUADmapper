#include "PlaybackTransport.hpp"

#include <algorithm>
#include <chrono>

namespace quadmapper {

PlaybackTransport::PlaybackTransport()
    : timeSource_([] { return std::chrono::steady_clock::now(); })
{
    clock_.setLoopMode(requestedMode_, now());
}

void PlaybackTransport::setTimeSource(TimeSource source) {
    if (source) timeSource_ = std::move(source);
}

void PlaybackTransport::load(double duration) {
    const PlaybackTimePoint t = now();
    clock_.setDuration(duration, t);
    if (!locked_) {
        state_ = PlaybackState::STOPPED;
        clock_.pause(t);
        clock_.seek(0.0, t);
    }
    reposition_ = true;
}

void PlaybackTransport::setDuration(double duration) {
    clock_.setDuration(duration, now());
    reposition_ = true;
}

void PlaybackTransport::play() {
    if (locked_) {
        // The master kept going while this source was paused: pick it up where it is.
        if (state_ != PlaybackState::PLAYING) reposition_ = true;
        state_ = PlaybackState::PLAYING;
        return;
    }
    if (state_ == PlaybackState::FINISHED) seek(0.0);
    state_ = PlaybackState::PLAYING;
    clock_.play(now());
}

void PlaybackTransport::pause() {
    if (state_ != PlaybackState::PLAYING) return;
    state_ = PlaybackState::PAUSED;
    if (!locked_) clock_.pause(now());
}

void PlaybackTransport::stop() {
    if (locked_) {
        pause();
        return;
    }
    state_ = PlaybackState::STOPPED;
    clock_.pause(now());
    seek(0.0);
}

void PlaybackTransport::seek(double seconds) {
    if (locked_) return;
    clock_.seek(seconds, now());
    if (state_ == PlaybackState::FINISHED) state_ = PlaybackState::PAUSED;
    reposition_ = true;
}

void PlaybackTransport::setSpeed(float speed) {
    speed_ = std::clamp(speed, 0.1f, 4.0f);
    if (!locked_) clock_.setSpeed(speed_, now());
}

void PlaybackTransport::setLoopMode(LoopMode mode, const std::string& reverseUnavailable) {
    requestedMode_ = mode;
    notice_ = mode == LoopMode::PING_PONG ? reverseUnavailable : std::string();
    applyLoopMode();
}

void PlaybackTransport::reverseUnavailable(const std::string& reason) {
    if (requestedMode_ != LoopMode::PING_PONG || !notice_.empty() || reason.empty()) return;
    notice_ = reason;
    applyLoopMode();
}

LoopMode PlaybackTransport::effectiveLoopMode() const {
    return requestedMode_ == LoopMode::PING_PONG && !notice_.empty() ? LoopMode::LOOP
                                                                     : requestedMode_;
}

void PlaybackTransport::applyLoopMode() {
    const LoopMode effective = effectiveLoopMode();
    if (clock_.loopMode() == effective) return;
    clock_.setLoopMode(effective, now());
    reposition_ = true;
}

void PlaybackTransport::finish() {
    if (locked_) return;
    state_ = PlaybackState::FINISHED;
    clock_.pause(now());
}

void PlaybackTransport::setTimelineLocked(bool locked, const PlaybackClock& master) {
    if (locked == locked_) return;
    const PlaybackTimePoint t = now();
    locked_ = locked;
    if (locked) {
        // A locked source follows the master; with its own transport disabled it
        // would otherwise have no way to start.
        state_ = PlaybackState::PLAYING;
        clock_.follow(master, t);
        reposition_ = true;
        return;
    }
    // Carry on from the master's position on this source's own clock.
    clock_.stopFollowing();
    clock_.setDirection(1, t);
    clock_.setSpeed(speed_, t);
    if (state_ == PlaybackState::PLAYING) clock_.play(t); else clock_.pause(t);
}

void PlaybackTransport::followTimeline(const PlaybackClock& master) {
    if (locked_ && clock_.follow(master, now())) reposition_ = true;
}

bool PlaybackTransport::takeReposition() {
    const bool pending = reposition_;
    reposition_ = false;
    return pending;
}

}  // namespace quadmapper
