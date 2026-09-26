#pragma once

#include "PlaybackClock.hpp"
#include "core/Types.hpp"

#include <functional>
#include <string>

namespace quadmapper {

/**
 * @brief Transport state shared by VideoPlayer and HapFileSource (ADR-050)
 *
 * Owns the source's PlaybackClock, its play state, the requested and effective loop
 * mode, and timeline lock. Main thread only.
 *
 * Frames are scheduled on the clock's timeline, so anything that changes which
 * timeline instant maps to which clip frame (a seek, a loop mode change, locking,
 * a master restart) makes frames already decoded wrong. Those raise a reposition
 * request; the owner calls takeReposition() on each update and, when it returns
 * true, restarts its decoder at clock().timeline(now()). Play, pause and speed only
 * move the anchor and never need one.
 *
 * While locked to the master timeline the clock follows the master; seek, stop and
 * speed are ignored, and play/pause only decide whether the source takes frames
 * (pause-while-unseen). A locked source never finishes: in Once mode it holds its
 * last frame until the master restarts.
 */
class PlaybackTransport {
public:
    using TimeSource = std::function<PlaybackTimePoint()>;

    PlaybackTransport();

    // Tests inject a clock; the default is std::chrono::steady_clock.
    void setTimeSource(TimeSource source);
    PlaybackTimePoint now() const { return timeSource_(); }

    // A clip was opened: stopped at 0 with this duration (0 = unknown). Speed, loop
    // mode and lock carry over.
    void load(double duration);
    // The duration was learned after opening.
    void setDuration(double duration);

    void play();
    void pause();
    void stop();
    void seek(double seconds);
    void setSpeed(float speed);  // clamped to 0.1 - 4.0

    // reverseUnavailable: why this file cannot play backwards (empty = it can). A
    // requested Ping-Pong then plays as Loop and loopModeNotice() says why.
    void setLoopMode(LoopMode mode, const std::string& reverseUnavailable = {});
    // The decoder found mid-play that Ping-Pong cannot run: fall back to Loop.
    void reverseUnavailable(const std::string& reason);
    // A Once clip showed its last frame.
    void finish();

    void setTimelineLocked(bool locked, const PlaybackClock& master);
    // Call before each update while locked.
    void followTimeline(const PlaybackClock& master);

    PlaybackState state() const { return state_; }
    bool timelineLocked() const { return locked_; }
    const PlaybackClock& clock() const { return clock_; }
    float speed() const { return speed_; }
    LoopMode loopMode() const { return requestedMode_; }  // what the user chose
    LoopMode effectiveLoopMode() const;                   // what plays
    const std::string& loopModeNotice() const { return notice_; }

    // True once after the decoded frames stopped matching the clock.
    bool takeReposition();

private:
    void applyLoopMode();

    TimeSource timeSource_;
    PlaybackClock clock_;
    PlaybackState state_ = PlaybackState::STOPPED;
    float speed_ = 1.0f;
    LoopMode requestedMode_ = LoopMode::LOOP;
    std::string notice_;  // non-empty: requested Ping-Pong plays as Loop, and why
    bool locked_ = false;
    bool reposition_ = false;
};

}  // namespace quadmapper
