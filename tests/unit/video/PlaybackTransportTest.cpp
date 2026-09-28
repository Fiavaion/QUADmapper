// PlaybackTransport (ADR-050): the player state machine shared by VideoPlayer and
// HapFileSource. Driven with injected time, so it runs without a decoder or sleeps.

#include <gtest/gtest.h>

#include "video/PlaybackTransport.hpp"

#include <chrono>

using namespace quadmapper;

namespace {

class PlaybackTransportTest : public testing::Test {
protected:
    void SetUp() override {
        transport_.setTimeSource([this] { return now_; });
        transport_.load(10.0);
        ASSERT_TRUE(transport_.takeReposition());  // a load positions the decoder
    }

    void advance(double seconds) {
        now_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(seconds));
    }

    double position() const { return transport_.clock().position(now_); }

    PlaybackTimePoint now_ = PlaybackTimePoint{} + std::chrono::hours(5);
    PlaybackTransport transport_;
};

}  // namespace

TEST_F(PlaybackTransportTest, LoadsStoppedAtTheStart) {
    EXPECT_EQ(transport_.state(), PlaybackState::STOPPED);
    advance(3.0);
    EXPECT_DOUBLE_EQ(position(), 0.0);
    EXPECT_EQ(transport_.loopMode(), LoopMode::LOOP);  // the default
    EXPECT_FALSE(transport_.takeReposition());
}

TEST_F(PlaybackTransportTest, PlayPauseAndSpeedNeverReposition) {
    transport_.play();
    advance(1.0);
    transport_.setSpeed(2.0f);
    advance(1.0);
    transport_.pause();
    advance(5.0);
    EXPECT_DOUBLE_EQ(position(), 3.0);
    transport_.play();
    advance(0.5);
    EXPECT_DOUBLE_EQ(position(), 4.0);
    EXPECT_FALSE(transport_.takeReposition());
}

TEST_F(PlaybackTransportTest, SpeedIsClamped) {
    transport_.setSpeed(9.0f);
    EXPECT_FLOAT_EQ(transport_.speed(), 4.0f);
    transport_.setSpeed(0.0f);
    EXPECT_FLOAT_EQ(transport_.speed(), 0.1f);
}

TEST_F(PlaybackTransportTest, SeekRepositionsAndKeepsPlaying) {
    transport_.play();
    advance(1.0);
    transport_.seek(7.0);
    EXPECT_TRUE(transport_.takeReposition());
    advance(1.5);
    EXPECT_DOUBLE_EQ(position(), 8.5);
    EXPECT_EQ(transport_.state(), PlaybackState::PLAYING);
}

TEST_F(PlaybackTransportTest, LoopWrapsWithoutRestartingTheClock) {
    transport_.play();
    advance(25.0);
    EXPECT_DOUBLE_EQ(position(), 5.0);
    EXPECT_EQ(transport_.state(), PlaybackState::PLAYING);
    EXPECT_FALSE(transport_.takeReposition());  // the decoder follows the plan itself
}

TEST_F(PlaybackTransportTest, StopReturnsToTheStart) {
    transport_.play();
    advance(4.0);
    transport_.stop();
    EXPECT_EQ(transport_.state(), PlaybackState::STOPPED);
    EXPECT_TRUE(transport_.takeReposition());
    advance(2.0);
    EXPECT_DOUBLE_EQ(position(), 0.0);
}

TEST_F(PlaybackTransportTest, OnceFinishesAndPlayRestarts) {
    transport_.setLoopMode(LoopMode::ONCE);
    EXPECT_TRUE(transport_.takeReposition());
    transport_.play();
    advance(12.0);
    EXPECT_TRUE(transport_.clock().sample(now_).ended);
    transport_.finish();
    EXPECT_EQ(transport_.state(), PlaybackState::FINISHED);
    advance(1.0);
    EXPECT_DOUBLE_EQ(position(), 10.0);

    transport_.play();  // from the end: start over
    EXPECT_EQ(transport_.state(), PlaybackState::PLAYING);
    EXPECT_TRUE(transport_.takeReposition());
    advance(2.0);
    EXPECT_DOUBLE_EQ(position(), 2.0);
}

TEST_F(PlaybackTransportTest, SeekFromFinishedPauses) {
    transport_.setLoopMode(LoopMode::ONCE);
    transport_.play();
    advance(11.0);
    transport_.finish();
    transport_.seek(3.0);
    EXPECT_EQ(transport_.state(), PlaybackState::PAUSED);
    advance(1.0);
    EXPECT_DOUBLE_EQ(position(), 3.0);
}

TEST_F(PlaybackTransportTest, PingPongReflects) {
    transport_.setLoopMode(LoopMode::PING_PONG);
    EXPECT_TRUE(transport_.takeReposition());
    EXPECT_EQ(transport_.effectiveLoopMode(), LoopMode::PING_PONG);
    EXPECT_TRUE(transport_.loopModeNotice().empty());
    transport_.play();
    advance(13.0);
    EXPECT_DOUBLE_EQ(position(), 7.0);
    EXPECT_EQ(transport_.clock().direction(now_), -1);
}

TEST_F(PlaybackTransportTest, UnplayableReverseFallsBackToLoopWithAReason) {
    transport_.setLoopMode(LoopMode::PING_PONG, "keyframes too far apart");
    EXPECT_EQ(transport_.loopMode(), LoopMode::PING_PONG);  // still what the user chose
    EXPECT_EQ(transport_.effectiveLoopMode(), LoopMode::LOOP);
    EXPECT_EQ(transport_.loopModeNotice(), "keyframes too far apart");
    EXPECT_FALSE(transport_.takeReposition());  // Loop already played: nothing to redo
    transport_.play();
    advance(13.0);
    EXPECT_DOUBLE_EQ(position(), 3.0);

    transport_.setLoopMode(LoopMode::ONCE);
    EXPECT_TRUE(transport_.loopModeNotice().empty());
}

TEST_F(PlaybackTransportTest, MidPlayReverseFailureKeepsThePosition) {
    transport_.setLoopMode(LoopMode::PING_PONG);
    transport_.takeReposition();
    transport_.play();
    advance(12.5);  // reverse leg at 7.5
    transport_.reverseUnavailable("group of pictures too large");
    EXPECT_EQ(transport_.effectiveLoopMode(), LoopMode::LOOP);
    EXPECT_TRUE(transport_.takeReposition());
    EXPECT_DOUBLE_EQ(position(), 7.5);
    advance(1.0);
    EXPECT_DOUBLE_EQ(position(), 8.5);  // forward now
    transport_.reverseUnavailable("again");  // already fallen back: ignored
    EXPECT_EQ(transport_.loopModeNotice(), "group of pictures too large");
    EXPECT_FALSE(transport_.takeReposition());
}

TEST_F(PlaybackTransportTest, ReverseFailureOutsidePingPongIsIgnored) {
    transport_.reverseUnavailable("irrelevant");
    EXPECT_TRUE(transport_.loopModeNotice().empty());
    EXPECT_FALSE(transport_.takeReposition());
}

TEST_F(PlaybackTransportTest, ModeSwitchWhilePlayingKeepsThePosition) {
    transport_.play();
    advance(3.0);
    transport_.setLoopMode(LoopMode::PING_PONG);
    EXPECT_TRUE(transport_.takeReposition());
    EXPECT_DOUBLE_EQ(position(), 3.0);
    advance(9.0);  // 12: reflected to 8
    EXPECT_DOUBLE_EQ(position(), 8.0);
    transport_.setLoopMode(LoopMode::LOOP);
    EXPECT_TRUE(transport_.takeReposition());
    advance(1.0);
    EXPECT_DOUBLE_EQ(position(), 9.0);
}

TEST_F(PlaybackTransportTest, UnknownDurationLearnedLater) {
    transport_.load(0.0);
    transport_.takeReposition();
    transport_.play();
    advance(12.0);
    EXPECT_DOUBLE_EQ(position(), 12.0);  // no wrapping yet
    transport_.setDuration(10.0);
    transport_.seek(0.0);
    EXPECT_TRUE(transport_.takeReposition());
    advance(11.0);
    EXPECT_DOUBLE_EQ(position(), 1.0);
}

// --- Timeline lock ---

TEST_F(PlaybackTransportTest, LockFollowsTheMaster) {
    PlaybackClock master;
    master.play(now_);
    advance(23.0);

    transport_.setTimelineLocked(true, master);
    EXPECT_TRUE(transport_.timelineLocked());
    EXPECT_EQ(transport_.state(), PlaybackState::PLAYING);
    EXPECT_TRUE(transport_.takeReposition());
    EXPECT_DOUBLE_EQ(position(), 3.0);

    // Own transport is ignored while locked.
    transport_.seek(8.0);
    transport_.setSpeed(3.0f);
    transport_.stop();  // pauses taking frames, does not move the timeline
    EXPECT_FALSE(transport_.takeReposition());
    advance(1.0);
    EXPECT_DOUBLE_EQ(position(), 4.0);
    EXPECT_FLOAT_EQ(transport_.speed(), 3.0f);  // remembered for after unlocking
}

TEST_F(PlaybackTransportTest, LockedSourcePausedWhileUnseenKeepsMasterTime) {
    PlaybackClock master;
    master.play(now_);
    transport_.setTimelineLocked(true, master);
    transport_.takeReposition();

    advance(2.0);
    transport_.pause();  // pause-while-unseen
    EXPECT_EQ(transport_.state(), PlaybackState::PAUSED);
    advance(5.0);
    transport_.followTimeline(master);
    EXPECT_DOUBLE_EQ(position(), 7.0);  // still master time
    EXPECT_FALSE(transport_.takeReposition());

    transport_.play();  // back on screen: the decoder jumps to where the master is
    EXPECT_TRUE(transport_.takeReposition());
    EXPECT_DOUBLE_EQ(position(), 7.0);
}

TEST_F(PlaybackTransportTest, MasterRestartRepositionsLockedSources) {
    PlaybackClock master;
    master.play(now_);
    transport_.setTimelineLocked(true, master);
    transport_.takeReposition();
    advance(4.0);

    master.pause(now_);
    transport_.followTimeline(master);
    EXPECT_FALSE(transport_.takeReposition());  // pause is continuous
    advance(1.0);
    EXPECT_DOUBLE_EQ(position(), 4.0);

    master.seek(0.0, now_);
    master.play(now_);
    transport_.followTimeline(master);
    EXPECT_TRUE(transport_.takeReposition());
    EXPECT_DOUBLE_EQ(position(), 0.0);
}

TEST_F(PlaybackTransportTest, LockedOnceHoldsTheLastFrameAndNeverFinishes) {
    transport_.setLoopMode(LoopMode::ONCE);
    PlaybackClock master;
    master.play(now_);
    transport_.setTimelineLocked(true, master);
    advance(30.0);
    transport_.finish();
    EXPECT_EQ(transport_.state(), PlaybackState::PLAYING);
    EXPECT_DOUBLE_EQ(position(), 10.0);
}

TEST_F(PlaybackTransportTest, UnlockContinuesOnTheOwnClock) {
    transport_.setSpeed(2.0f);
    PlaybackClock master;
    master.play(now_);
    advance(1.0);
    transport_.setTimelineLocked(true, master);
    transport_.takeReposition();
    advance(2.0);  // master 3.0, at 1x

    transport_.setTimelineLocked(false, master);
    EXPECT_FALSE(transport_.timelineLocked());
    EXPECT_FALSE(transport_.takeReposition());  // continuous
    EXPECT_DOUBLE_EQ(position(), 3.0);
    advance(1.0);
    EXPECT_DOUBLE_EQ(position(), 5.0);  // own speed again
    transport_.seek(1.0);  // own transport works again
    EXPECT_TRUE(transport_.takeReposition());
    EXPECT_DOUBLE_EQ(position(), 1.0);
}

TEST_F(PlaybackTransportTest, LockedPingPongReflectsMasterTime) {
    transport_.setLoopMode(LoopMode::PING_PONG);
    PlaybackClock master;
    master.play(now_);
    transport_.setTimelineLocked(true, master);
    advance(14.0);
    EXPECT_DOUBLE_EQ(position(), 6.0);
    EXPECT_EQ(transport_.clock().direction(now_), -1);
}
