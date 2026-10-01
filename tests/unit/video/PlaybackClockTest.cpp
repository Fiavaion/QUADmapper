// PlaybackClock (ADR-050 §1): positions computed from an anchor with injected time,
// so every expectation is exact and no test sleeps.

#include <gtest/gtest.h>

#include "video/PlaybackClock.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <random>

using namespace quadmapper;

namespace {

// Tolerance for results that go through one floating-point division.
constexpr double EXACT = 1e-9;

const PlaybackTimePoint T0 = PlaybackTimePoint{} + std::chrono::hours(1000);

PlaybackTimePoint at(double seconds) {
    return T0 + std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::duration<double>(seconds));
}

PlaybackClock clockWith(double duration, LoopMode mode) {
    PlaybackClock clock;
    clock.setDuration(duration, T0);
    clock.setLoopMode(mode, T0);
    return clock;
}

}  // namespace

// --- Transport re-anchoring ---

TEST(PlaybackClock, StoppedClockHoldsItsPosition) {
    PlaybackClock clock;
    EXPECT_FALSE(clock.running());
    EXPECT_EQ(clock.timeline(T0), 0.0);
    EXPECT_EQ(clock.timeline(at(3600.0)), 0.0);
}

TEST(PlaybackClock, PlayFollowsTheWallClock) {
    PlaybackClock clock;
    clock.play(T0);
    EXPECT_TRUE(clock.running());
    EXPECT_DOUBLE_EQ(clock.timeline(at(2.5)), 2.5);
    EXPECT_DOUBLE_EQ(clock.timeline(at(10.0)), 10.0);
}

TEST(PlaybackClock, PauseFreezesAndResumeContinuesExactly) {
    PlaybackClock clock;
    clock.play(T0);
    clock.pause(at(1.25));
    EXPECT_FALSE(clock.running());
    EXPECT_DOUBLE_EQ(clock.timeline(at(10.0)), 1.25);
    clock.play(at(10.0));
    EXPECT_DOUBLE_EQ(clock.timeline(at(11.0)), 2.25);
    clock.play(at(11.0));  // already running: re-anchoring changes nothing
    EXPECT_DOUBLE_EQ(clock.timeline(at(12.0)), 3.25);
}

TEST(PlaybackClock, SeekReanchorsExactly) {
    PlaybackClock clock = clockWith(10.0, LoopMode::LOOP);
    clock.play(T0);
    clock.seek(3.0, at(1.0));
    EXPECT_DOUBLE_EQ(clock.position(at(1.0)), 3.0);
    EXPECT_DOUBLE_EQ(clock.position(at(2.0)), 4.0);
    clock.seek(-5.0, at(2.0));  // clamped to the clip
    EXPECT_DOUBLE_EQ(clock.position(at(2.0)), 0.0);
    clock.pause(at(2.5));
    clock.seek(7.5, at(4.0));  // seeking while paused stays paused
    EXPECT_DOUBLE_EQ(clock.position(at(9.0)), 7.5);
}

TEST(PlaybackClock, SpeedChangeReanchorsExactly) {
    PlaybackClock clock;
    clock.play(T0);
    clock.setSpeed(2.0, at(1.0));
    EXPECT_DOUBLE_EQ(clock.timeline(at(3.0)), 5.0);
    clock.setSpeed(0.1, at(3.0));
    EXPECT_NEAR(clock.timeline(at(13.0)), 6.0, EXACT);
    // Non-positive or NaN speeds are ignored.
    clock.setSpeed(0.0, at(13.0));
    clock.setSpeed(-1.0, at(13.0));
    clock.setSpeed(std::numeric_limits<double>::quiet_NaN(), at(13.0));
    EXPECT_DOUBLE_EQ(clock.speed(), 0.1);
}

TEST(PlaybackClock, DirectionChangeReanchorsExactly) {
    PlaybackClock clock;
    clock.seek(5.0, T0);
    clock.play(T0);
    clock.setDirection(-1, at(2.0));
    EXPECT_EQ(clock.baseDirection(), -1);
    EXPECT_DOUBLE_EQ(clock.timeline(at(2.0)), 7.0);
    EXPECT_DOUBLE_EQ(clock.timeline(at(5.0)), 4.0);
    EXPECT_EQ(clock.direction(at(5.0)), -1);
    clock.setDirection(0, at(5.0));  // ignored
    clock.setDirection(2, at(5.0));  // ignored
    EXPECT_EQ(clock.baseDirection(), -1);
    clock.setDirection(1, at(5.0));
    EXPECT_DOUBLE_EQ(clock.timeline(at(6.0)), 5.0);
}

// --- Loop and Ping-Pong mapping ---

TEST(PlaybackClock, LoopWrapsAnalytically) {
    PlaybackClock clock = clockWith(2.0, LoopMode::LOOP);
    clock.play(T0);
    const ClipPosition clip = clock.sample(at(5.5));
    EXPECT_DOUBLE_EQ(clip.position, 1.5);
    EXPECT_EQ(clip.leg, 2);
    EXPECT_EQ(clip.direction, 1);
    EXPECT_FALSE(clip.ended);
    EXPECT_DOUBLE_EQ(clock.position(at(2.0)), 0.0);  // the loop point is the start
}

TEST(PlaybackClock, ManyWrapsInOneStepAtFourTimesSpeed) {
    PlaybackClock clock = clockWith(0.04, LoopMode::LOOP);
    clock.setSpeed(4.0, T0);
    clock.play(T0);
    const double elapsed = 1000.013;
    const double timeline = elapsed * 4.0;
    const ClipPosition clip = clock.sample(at(elapsed));
    EXPECT_NEAR(clip.position, std::fmod(timeline, 0.04), EXACT);
    EXPECT_EQ(clip.leg, static_cast<int64_t>(std::floor(timeline / 0.04)));
    EXPECT_GE(clip.position, 0.0);
    EXPECT_LE(clip.position, 0.04);
}

TEST(PlaybackClock, PingPongReflectsAtEachEnd) {
    PlaybackClock clock = clockWith(2.0, LoopMode::PING_PONG);
    clock.play(T0);
    struct Expect { double t, position; int direction; int64_t leg; };
    for (const Expect& e : {Expect{0.5, 0.5, 1, 0}, Expect{1.999, 1.999, 1, 0},
                            Expect{2.5, 1.5, -1, 1}, Expect{3.999, 0.001, -1, 1},
                            Expect{4.5, 0.5, 1, 2}, Expect{6.25, 1.75, -1, 3}}) {
        const ClipPosition clip = clock.sample(at(e.t));
        EXPECT_NEAR(clip.position, e.position, EXACT) << "t=" << e.t;
        EXPECT_EQ(clip.direction, e.direction) << "t=" << e.t;
        EXPECT_EQ(clip.leg, e.leg) << "t=" << e.t;
    }
}

TEST(PlaybackClock, PingPongManyReflectionsInOneStep) {
    PlaybackClock clock = clockWith(2.0, LoopMode::PING_PONG);
    clock.setSpeed(4.0, T0);
    clock.play(T0);
    // 250.25 s at 4x = timeline 1001: leg 500 (forward), 1 s in.
    ClipPosition clip = clock.sample(at(250.25));
    EXPECT_NEAR(clip.position, 1.0, EXACT);
    EXPECT_EQ(clip.direction, 1);
    EXPECT_EQ(clip.leg, 500);
    // Timeline 1003.5: leg 501 (reverse), 1.5 s into it.
    clip = clock.sample(at(250.875));
    EXPECT_NEAR(clip.position, 0.5, EXACT);
    EXPECT_EQ(clip.direction, -1);
}

TEST(PlaybackClock, OnceClampsAtTheEnd) {
    PlaybackClock clock = clockWith(2.0, LoopMode::ONCE);
    clock.play(T0);
    EXPECT_FALSE(clock.sample(at(1.0)).ended);
    ClipPosition clip = clock.sample(at(2.0));
    EXPECT_TRUE(clip.ended);
    EXPECT_DOUBLE_EQ(clip.position, 2.0);
    clip = clock.sample(at(50.0));
    EXPECT_TRUE(clip.ended);
    EXPECT_DOUBLE_EQ(clip.position, 2.0);
    EXPECT_EQ(clip.leg, 0);
}

TEST(PlaybackClock, TinyZeroAndInvalidDurationsDoNotWrap) {
    for (double duration : {0.0, 1e-12, 1e-4, -3.0, std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity()}) {
        for (LoopMode mode : {LoopMode::ONCE, LoopMode::LOOP, LoopMode::PING_PONG}) {
            PlaybackClock clock = clockWith(duration, mode);
            clock.play(T0);
            const ClipPosition clip = clock.sample(at(12.5));
            EXPECT_DOUBLE_EQ(clip.position, 12.5) << "duration " << duration;
            EXPECT_EQ(clip.leg, 0);
            EXPECT_EQ(clip.direction, 1);
            EXPECT_FALSE(clip.ended);
            EXPECT_EQ(clock.discontinuity(at(12.5)), 0u);
            const PlaybackLeg leg = legAt(12.5, duration, mode);
            EXPECT_TRUE(leg.final);
            EXPECT_EQ(leg.start, 0.0);
        }
    }
}

TEST(PlaybackClock, SmallestWrappingDurationStillWraps) {
    PlaybackClock clock = clockWith(MIN_WRAP_DURATION, LoopMode::LOOP);
    clock.play(T0);
    const ClipPosition clip = clock.sample(at(1.0005));
    EXPECT_NEAR(clip.position, 0.0005, 1e-9);
    EXPECT_EQ(clip.leg, 1000);
}

// --- Drift ---

// An hour of frames at 60 Hz, sampled at nanosecond timestamps: the clock is exact at
// every frame because each position comes from the anchor, never from a running sum.
TEST(PlaybackClock, SimulatedHourHasNoDrift) {
    constexpr double DURATION = 7.3;
    PlaybackClock clock = clockWith(DURATION, LoopMode::LOOP);
    clock.play(T0);

    const std::chrono::nanoseconds frame{16'666'667};  // 1/60 s, rounded as a display would
    PlaybackTimePoint now = T0;
    double worst = 0.0;
    for (int i = 1; i <= 60 * 3600; ++i) {
        now += frame;
        const double elapsed = std::chrono::duration<double>(now - T0).count();
        worst = std::max(worst, std::abs(clock.timeline(now) - elapsed));
    }
    EXPECT_LT(worst, 1e-9);
    const double elapsed = std::chrono::duration<double>(now - T0).count();
    EXPECT_NEAR(clock.position(now), std::fmod(elapsed, DURATION), 1e-9);
    // One frame at 60 fps is 16.7 ms; the PRD allows < 1 frame per hour.
    EXPECT_LT(std::abs(clock.timeline(now) - elapsed), 1e-9);
}

TEST(PlaybackClock, HourOfPausesAndSpeedChangesStaysExact) {
    PlaybackClock clock = clockWith(2.5, LoopMode::PING_PONG);
    std::mt19937 rng(7);
    double expected = 0.0;  // the timeline summed from the running intervals
    PlaybackTimePoint now = T0;
    bool running = false;
    double speed = 1.0;
    while (now - T0 < std::chrono::hours(1)) {
        const std::chrono::milliseconds gap(500 + rng() % 4500);
        if (running) expected += std::chrono::duration<double>(gap).count() * speed;
        now += gap;
        if (rng() % 3 == 0) {
            speed = 0.1 + (rng() % 40) / 10.0;
            clock.setSpeed(speed, now);
        } else if (running) {
            clock.pause(now);
            running = false;
        } else {
            clock.play(now);
            running = true;
        }
    }
    // Both sides round once per interval; neither loses time between them.
    EXPECT_NEAR(clock.timeline(now), expected, 1e-8);
    const ClipPosition clip = clock.sample(now);
    EXPECT_NEAR(clip.position, mapTimeline(expected, 2.5, LoopMode::PING_PONG).position, 1e-8);
}

// --- Mode, duration and seek semantics ---

TEST(PlaybackClock, SeekOnReverseLegKeepsReversing) {
    PlaybackClock clock = clockWith(2.0, LoopMode::PING_PONG);
    clock.play(T0);
    clock.seek(0.5, at(2.5));  // on the reverse leg
    ClipPosition clip = clock.sample(at(2.5));
    EXPECT_DOUBLE_EQ(clip.position, 0.5);
    EXPECT_EQ(clip.direction, -1);
    clip = clock.sample(at(2.75));
    EXPECT_DOUBLE_EQ(clip.position, 0.25);
    EXPECT_EQ(clip.direction, -1);
    // It reflects at 0 and plays forward again.
    clip = clock.sample(at(3.25));
    EXPECT_DOUBLE_EQ(clip.position, 0.25);
    EXPECT_EQ(clip.direction, 1);
}

TEST(PlaybackClock, LoopModeChangeKeepsThePosition) {
    PlaybackClock clock = clockWith(2.0, LoopMode::LOOP);
    clock.play(T0);
    clock.setLoopMode(LoopMode::PING_PONG, at(5.5));  // loop pass 2, 1.5 s in
    ClipPosition clip = clock.sample(at(5.5));
    EXPECT_DOUBLE_EQ(clip.position, 1.5);
    EXPECT_EQ(clip.direction, 1);
    EXPECT_DOUBLE_EQ(clock.position(at(6.0)), 2.0);        // reaches the end...
    clip = clock.sample(at(6.3));
    EXPECT_NEAR(clip.position, 1.7, EXACT);                // ...and reflects
    EXPECT_EQ(clip.direction, -1);

    clock.setLoopMode(LoopMode::LOOP, at(6.3));  // reverse leg -> forward from there
    clip = clock.sample(at(6.3));
    EXPECT_NEAR(clip.position, 1.7, EXACT);
    EXPECT_EQ(clip.direction, 1);
    EXPECT_NEAR(clock.position(at(6.5)), 1.9, EXACT);

    clock.setLoopMode(LoopMode::ONCE, at(6.5));
    EXPECT_NEAR(clock.position(at(6.5)), 1.9, EXACT);
    EXPECT_TRUE(clock.sample(at(6.7)).ended);
}

TEST(PlaybackClock, DurationChangeKeepsThePosition) {
    PlaybackClock clock = clockWith(10.0, LoopMode::LOOP);
    clock.play(T0);
    clock.setDuration(4.0, at(3.0));
    EXPECT_DOUBLE_EQ(clock.position(at(3.0)), 3.0);
    EXPECT_DOUBLE_EQ(clock.position(at(4.5)), 0.5);  // now wraps at 4
    clock.setDuration(0.25, at(4.5));                  // shorter than the position: clamped
    EXPECT_LE(clock.position(at(4.5)), 0.25);
}

// A clip played while its file opens (TD-35) runs with no duration; when the length
// arrives longer ago than the clip lasts, Loop and Ping-Pong carry on where they would
// have been had it been known from the start, not from the clip's end or its start.
TEST(PlaybackClock, ADurationArrivingLateWrapsTheTimeRunMeanwhile) {
    PlaybackClock loop = clockWith(0.0, LoopMode::LOOP);
    loop.play(T0);
    loop.setDuration(2.0, at(4.5));
    EXPECT_NEAR(loop.position(at(4.5)), 0.5, EXACT);
    EXPECT_EQ(loop.direction(at(4.5)), 1);
    EXPECT_NEAR(loop.position(at(5.0)), 1.0, EXACT);

    PlaybackClock pingPong = clockWith(0.0, LoopMode::PING_PONG);
    pingPong.play(T0);
    pingPong.setDuration(2.0, at(3.0));  // one second into the first reverse leg
    EXPECT_NEAR(pingPong.position(at(3.0)), 1.0, EXACT);
    EXPECT_EQ(pingPong.direction(at(3.0)), -1);
    EXPECT_NEAR(pingPong.position(at(3.5)), 0.5, EXACT);

    PlaybackClock once = clockWith(0.0, LoopMode::ONCE);  // Once still stops at the end
    once.play(T0);
    once.setDuration(2.0, at(4.5));
    EXPECT_NEAR(once.position(at(4.5)), 2.0, EXACT);
    EXPECT_TRUE(once.sample(at(4.5)).ended);
}

// --- Discontinuities ---

TEST(PlaybackClock, DiscontinuityCountsSeeksWrapsAndReflections) {
    PlaybackClock clock = clockWith(1.0, LoopMode::LOOP);
    clock.play(T0);
    EXPECT_EQ(clock.discontinuity(at(0.5)), 0u);
    EXPECT_EQ(clock.discontinuity(at(1.5)), 1u);
    EXPECT_EQ(clock.discontinuity(at(3.2)), 3u);
    clock.seek(0.5, at(3.2));
    EXPECT_EQ(clock.discontinuity(at(3.2)), 4u);
    // Transport that keeps playback continuous does not count.
    clock.pause(at(3.3));
    clock.play(at(4.0));
    clock.setSpeed(2.0, at(4.0));
    EXPECT_EQ(clock.discontinuity(at(4.0)), 4u);
    // 0.6 in, 2x for 0.5 s: one more wrap.
    EXPECT_EQ(clock.discontinuity(at(4.5)), 5u);

    PlaybackClock pingPong = clockWith(1.0, LoopMode::PING_PONG);
    pingPong.play(T0);
    EXPECT_EQ(pingPong.discontinuity(at(2.5)), 2u);  // two reflections
}

TEST(PlaybackClock, DiscontinuityNeverDecreases) {
    PlaybackClock clock = clockWith(0.7, LoopMode::PING_PONG);
    std::mt19937 rng(11);
    uint64_t last = 0;
    double t = 0.0;
    for (int i = 0; i < 5000; ++i) {
        t += (rng() % 1000) / 1000.0;
        switch (rng() % 7) {
            case 0: clock.play(at(t)); break;
            case 1: clock.pause(at(t)); break;
            case 2: clock.seek((rng() % 70) / 100.0, at(t)); break;
            case 3: clock.setSpeed(0.1 + (rng() % 40) / 10.0, at(t)); break;
            case 4: clock.setDirection(rng() % 2 ? 1 : -1, at(t)); break;
            case 5: clock.setLoopMode(static_cast<LoopMode>(rng() % 3), at(t)); break;
            default: break;
        }
        const uint64_t now = clock.discontinuity(at(t));
        ASSERT_GE(now, last) << "step " << i;
        last = now;
    }
}

// --- Timeline lock: following a master ---

TEST(PlaybackClock, FollowerMapsMasterTimeOntoItsClip) {
    PlaybackClock master;  // no duration: it never wraps
    master.play(T0);

    PlaybackClock follower = clockWith(3.0, LoopMode::LOOP);
    EXPECT_TRUE(follower.follow(master, at(10.0)));  // locking is a jump
    EXPECT_TRUE(follower.following());
    EXPECT_DOUBLE_EQ(follower.position(at(10.0)), 1.0);
    EXPECT_FALSE(follower.follow(master, at(10.5)));  // nothing changed

    master.pause(at(11.0));
    EXPECT_FALSE(follower.follow(master, at(11.02)));  // a pause is continuous
    EXPECT_DOUBLE_EQ(follower.position(at(20.0)), 2.0);
    const uint64_t before = follower.discontinuity(at(20.0));

    master.seek(0.0, at(20.0));  // restart
    master.play(at(20.0));
    EXPECT_TRUE(follower.follow(master, at(20.0)));
    EXPECT_DOUBLE_EQ(follower.position(at(20.0)), 0.0);
    EXPECT_GT(follower.discontinuity(at(20.0)), before);
    EXPECT_DOUBLE_EQ(follower.position(at(21.5)), 1.5);

    follower.stopFollowing();
    EXPECT_FALSE(follower.following());
    EXPECT_TRUE(follower.follow(master, at(22.0)));  // relocking jumps again
}

TEST(PlaybackClock, FollowerModeChangeKeepsTheMasterTimeline) {
    PlaybackClock master;
    master.play(T0);
    PlaybackClock follower = clockWith(2.0, LoopMode::LOOP);
    follower.follow(master, at(3.5));
    EXPECT_DOUBLE_EQ(follower.position(at(3.5)), 1.5);
    follower.setLoopMode(LoopMode::PING_PONG, at(3.5));  // leg 1 reverses
    EXPECT_DOUBLE_EQ(follower.position(at(3.5)), 0.5);
    EXPECT_DOUBLE_EQ(follower.timeline(at(3.5)), master.timeline(at(3.5)));
}

// Two sources of different durations locked to one master stay phase-exact: after an
// hour with the master paused and resumed along the way, each position is exactly the
// master's time mapped onto its clip.
TEST(PlaybackClock, TwoLockedSourcesStayPhaseExactForAnHour) {
    PlaybackClock master;
    master.play(T0);
    PlaybackClock a = clockWith(2.7, LoopMode::LOOP);
    PlaybackClock b = clockWith(7.3, LoopMode::PING_PONG);
    a.follow(master, T0);
    b.follow(master, T0);

    double runningTime = 0.0;
    double t = 0.0;
    bool running = true;
    for (int i = 0; i < 400; ++i) {
        const double next = t + 9.0;
        if (running) runningTime += next - t;
        t = next;
        if (running) master.pause(at(t)); else master.play(at(t));
        running = !running;
        a.follow(master, at(t));
        b.follow(master, at(t));
        const double m = master.timeline(at(t));
        ASSERT_EQ(a.position(at(t)), mapTimeline(m, 2.7, LoopMode::LOOP).position);
        ASSERT_EQ(b.position(at(t)), mapTimeline(m, 7.3, LoopMode::PING_PONG).position);
    }
    EXPECT_NEAR(t, 3600.0, 1e-9);
    EXPECT_NEAR(master.timeline(at(t)), runningTime, 1e-9);
    // Checked against the analytic phase, independent of the clock's arithmetic.
    EXPECT_NEAR(a.position(at(t)), std::fmod(runningTime, 2.7), 1e-9);
    const double bPhase = std::fmod(runningTime, 14.6);
    EXPECT_NEAR(b.position(at(t)), bPhase <= 7.3 ? bPhase : 14.6 - bPhase, 1e-9);
}

// --- Legs and frame due times (what decoders schedule) ---

TEST(PlaybackLegs, LoopAndPingPongLegs) {
    PlaybackLeg leg = legAt(5.5, 2.0, LoopMode::LOOP);
    EXPECT_EQ(leg.index, 2);
    EXPECT_DOUBLE_EQ(leg.start, 4.0);
    EXPECT_EQ(leg.direction, 1);
    EXPECT_FALSE(leg.final);
    leg = nextLeg(leg, 2.0, LoopMode::LOOP);
    EXPECT_EQ(leg.index, 3);
    EXPECT_DOUBLE_EQ(leg.start, 6.0);

    leg = legAt(2.5, 2.0, LoopMode::PING_PONG);
    EXPECT_EQ(leg.direction, -1);
    leg = nextLeg(leg, 2.0, LoopMode::PING_PONG);
    EXPECT_EQ(leg.direction, 1);
    EXPECT_DOUBLE_EQ(leg.start, 4.0);

    leg = legAt(99.0, 2.0, LoopMode::ONCE);
    EXPECT_TRUE(leg.final);
    EXPECT_TRUE(nextLeg(leg, 2.0, LoopMode::ONCE).final);
}

// For any instant, the frame with the latest due time at or before it must be the frame
// whose interval holds the clock's clip position: forward and reverse legs alike.
TEST(PlaybackLegs, DueTimesSelectTheFrameAtTheClockPosition) {
    constexpr double DURATION = 1.0;
    constexpr int FRAMES = 10;
    for (LoopMode mode : {LoopMode::LOOP, LoopMode::PING_PONG}) {
        // Frames in the order a decoder emits them over three legs.
        struct Scheduled { double due; int frame; };
        std::vector<Scheduled> schedule;
        PlaybackLeg leg = legAt(0.0, DURATION, mode);
        for (int n = 0; n < 3; ++n, leg = nextLeg(leg, DURATION, mode)) {
            for (int i = 0; i < FRAMES; ++i) {
                const int frame = leg.direction > 0 ? i : FRAMES - 1 - i;
                const double pts = frame * 0.1;
                schedule.push_back({frameDueTime(leg, DURATION, pts, pts + 0.1), frame});
            }
        }
        for (size_t i = 1; i < schedule.size(); ++i) {
            ASSERT_GE(schedule[i].due, schedule[i - 1].due) << "not in timeline order";
        }
        for (double t = 0.005; t < 3.0; t += 0.01) {
            int shown = -1;
            for (const Scheduled& s : schedule) {
                if (s.due <= t + 1e-9) shown = s.frame;
            }
            const double position = mapTimeline(t, DURATION, mode).position;
            EXPECT_EQ(shown, std::min(FRAMES - 1, static_cast<int>(std::floor(position * FRAMES + 1e-9))))
                << "t=" << t;
        }
    }
}
