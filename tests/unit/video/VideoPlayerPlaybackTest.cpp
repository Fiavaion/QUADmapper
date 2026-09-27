// VideoPlayer on the playback clock (ADR-050), with real decoding of clips encoded at
// test time and injected clock time. At every step the test waits (in real time)
// until the player presents exactly the frame the clock schedules, then checks which
// frame that is: Loop replays the first frames after each wrap without restarting the
// clock, Ping-Pong's reverse leg presents every frame in descending order, and seeks,
// mode switches, pauses, speed and timeline lock all keep frame and clock together.

#include <gtest/gtest.h>

#include "video/VideoPlayer.hpp"
#include "video/VideoDecoder.hpp"
#include "video/FrameSelection.hpp"
#include "video/PlaybackClock.hpp"
#include "TestClips.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

using namespace quadmapper;
namespace fs = std::filesystem;

namespace {

constexpr int FPS = 25;
constexpr int FRAMES = 50;                   // 2 s
constexpr double DURATION = double(FRAMES) / FPS;
constexpr int64_t START_OFFSET_FRAMES = 35;  // like MPEG-TS: timestamps start at 1.4 s
constexpr double DUE_TOLERANCE = 1e-4;       // frames are 40 ms apart
const auto WAIT_LIMIT = std::chrono::seconds(10);  // real time allowed per step

struct ClipSpec {
    const char* extension;
    int gopSize;
};

void PrintTo(const ClipSpec& spec, std::ostream* os) {
    *os << spec.extension << " GOP " << spec.gopSize;
}

std::string clipName(const testing::TestParamInfo<ClipSpec>& info) {
    return std::string(info.param.extension + 1) + "_gop" + std::to_string(info.param.gopSize);
}

fs::path clipPath(const std::string& tag, const char* extension) {
    std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
    std::replace(name.begin(), name.end(), '/', '_');
    return fs::temp_directory_path() / ("qm_player_" + name + "_" + tag + extension);
}

int frameIndex(const VideoFrame& frame) {
    return static_cast<int>(std::lround(frame.pts * FPS));
}

// When the frame the clock shows at `now` became current: the scheduling rule of
// ADR-050 (forward legs: leg start + pts; reverse legs: when the position drops
// below the frame's end), computed independently of the decoder.
double expectedDue(const PlaybackClock& clock, PlaybackTimePoint now, int frames) {
    const double t = clock.timeline(now);
    const double duration = clock.duration();
    const PlaybackLeg leg = legAt(t, duration, clock.loopMode());
    const double position = clock.sample(now).position;
    // A frame counts as due FRAME_DUE_EPSILON early (FrameSelection.hpp).
    if (leg.direction > 0) {
        const int frame = std::clamp(static_cast<int>(std::floor((position + FRAME_DUE_EPSILON) * FPS)),
                                     0, frames - 1);
        return leg.start + static_cast<double>(frame) / FPS;
    }
    const int frame = std::clamp(static_cast<int>(std::ceil((position - FRAME_DUE_EPSILON) * FPS)) - 1,
                                 0, frames - 1);
    const double end = frame == frames - 1 ? duration : static_cast<double>(frame + 1) / FPS;
    return leg.start + duration - end;
}

// Drives one player on injected time.
class Driver {
public:
    explicit Driver(PlaybackTimePoint& now, int frames = FRAMES) : now_(now), frames_(frames) {
        player.setTimeSource([this] { return now_; });
    }

    // Set the clock to `seconds` after the start and wait until the player presents
    // the frame scheduled there. Returns its index (-1 if it never came).
    int showAt(double seconds, const PlaybackClock* master = nullptr) {
        now_ = start + std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::duration<double>(seconds));
        const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
        while (std::chrono::steady_clock::now() < deadline) {
            if (master) player.followTimeline(*master);
            player.update(0.0);
            if (player.hasNewFrame()) {
                presented.push_back(player.getCurrentFrame());
                player.consumeFrame();
            }
            const VideoFrame& frame = player.getCurrentFrame();
            if (frame.isValid() &&
                std::abs(frame.due - expectedDue(player.getClock(), now_, frames_)) < DUE_TOLERANCE) {
                return frameIndex(frame);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ADD_FAILURE() << "no frame for t=" << seconds << " (clip position "
                      << player.getClock().position(now_) << ", showing "
                      << (player.getCurrentFrame().isValid() ? frameIndex(player.getCurrentFrame()) : -1)
                      << " due " << player.getCurrentFrame().due << ")";
        return -1;
    }

    // Keep updating at the current clock time until `done` holds (real-time limit).
    template <typename Done>
    bool waitFor(Done&& done) {
        const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            player.update(0.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    // Frames shown at the middle of each frame interval from `from` to `to` seconds.
    std::vector<int> showRange(double from, double to, const PlaybackClock* master = nullptr) {
        std::vector<int> shown;
        for (double t = from; t < to - 1e-9; t += 1.0 / FPS) {
            shown.push_back(showAt(t, master));
            if (shown.back() < 0) break;
        }
        return shown;
    }

    VideoPlayer player;
    PlaybackTimePoint start;

private:
    PlaybackTimePoint& now_;
    int frames_;

public:
    std::vector<VideoFrame> presented;
};

std::vector<int> ascending(int from, int to) {  // inclusive
    std::vector<int> v;
    for (int i = from; i <= to; ++i) v.push_back(i);
    return v;
}

std::vector<int> descending(int from, int to) {  // inclusive
    std::vector<int> v;
    for (int i = from; i >= to; --i) v.push_back(i);
    return v;
}

std::vector<int> concat(std::initializer_list<std::vector<int>> parts) {
    std::vector<int> v;
    for (const auto& p : parts) v.insert(v.end(), p.begin(), p.end());
    return v;
}

class VideoPlayerPlayback : public testing::TestWithParam<ClipSpec> {
protected:
    void SetUp() override {
        path_ = clipPath("clip", GetParam().extension);
        if (!testclips::writeMpeg2Clip(path_, GetParam().gopSize, FRAMES, FPS, START_OFFSET_FRAMES)) {
            GTEST_SKIP() << "FFmpeg cannot write " << path_;
        }
        driver_.start = now_;
        ASSERT_TRUE(driver_.player.load(path_));
        // Matroska declares its end time (1.4 s start offset included) as its duration;
        // the decoder measures the real end instead.
        ASSERT_NEAR(driver_.player.getState().duration, DURATION, 1e-6);
    }

    void TearDown() override {
        driver_.player.unload();
        std::error_code ec;
        fs::remove(path_, ec);
    }

    // Frames shown for the middle of each frame interval over `passes` clip lengths.
    std::vector<int> playPasses(int passes) {
        return driver_.showRange(0.5 / FPS, passes * DURATION);
    }

    VideoPlayer& player() { return driver_.player; }

    fs::path path_;
    PlaybackTimePoint now_ = PlaybackTimePoint{} + std::chrono::hours(2);
    Driver driver_{now_};
};

}  // namespace

TEST_P(VideoPlayerPlayback, LoopShowsTheFirstFramesAfterEachWrapWithoutRestartingTheClock) {
    player().setLoopMode(LoopMode::LOOP);
    player().play();
    const uint64_t discontinuities = player().getClock().discontinuity(now_);
    const std::vector<int> shown = playPasses(3);
    const double duration = player().getClock().duration();
    ASSERT_FALSE(shown.empty());
    for (size_t i = 0; i < shown.size(); ++i) {
        const double t = (i + 0.5) / FPS;
        const int expected = std::min(FRAMES - 1, static_cast<int>(std::floor(std::fmod(t, duration) * FPS)));
        ASSERT_EQ(shown[i], expected) << "step " << i;
    }
    // The clock never restarted: its timeline is the elapsed time.
    const double elapsed = std::chrono::duration<double>(now_ - driver_.start).count();
    EXPECT_NEAR(player().getClock().timeline(now_), elapsed, 1e-9);
    EXPECT_EQ(player().getClock().discontinuity(now_) - discontinuities,
              static_cast<uint64_t>(std::floor(elapsed / duration)));  // one per wrap
    EXPECT_EQ(player().getState().state, PlaybackState::PLAYING);
    EXPECT_TRUE(player().getErrorMessage().empty());
}

TEST_P(VideoPlayerPlayback, PingPongReverseLegShowsEveryFrameDescending) {
    player().setLoopMode(LoopMode::PING_PONG);
    EXPECT_TRUE(player().getLoopModeNotice().empty()) << player().getLoopModeNotice();
    player().play();
    const std::vector<int> shown = playPasses(3);
    EXPECT_EQ(shown, concat({ascending(0, FRAMES - 1), descending(FRAMES - 1, 0),
                             ascending(0, FRAMES - 1)}));

    // The frames presented while the clock ran the reverse leg: strictly descending pts
    // (after the end frame shown again at the reflection), no frame skipped.
    std::vector<int> reverse;
    for (const VideoFrame& f : driver_.presented) {
        if (f.due >= DURATION - 1e-6 && f.due < 2 * DURATION - 1e-6) reverse.push_back(frameIndex(f));
    }
    EXPECT_EQ(reverse, descending(FRAMES - 1, 0));
    EXPECT_LE(player().getReverseCachePeakBytes(), DEFAULT_REVERSE_CACHE_BYTES);
    EXPECT_GT(player().getReverseCachePeakBytes(), 0u);
}

TEST_P(VideoPlayerPlayback, SeekDuringTheReverseLegKeepsReversing) {
    player().setLoopMode(LoopMode::PING_PONG);
    player().play();
    driver_.showRange(0.5 / FPS, DURATION + 0.3);  // into the reverse leg
    ASSERT_EQ(player().getClock().direction(now_), -1);

    player().seek(0.8);  // the start of frame 20
    const double seekedAt = std::chrono::duration<double>(now_ - driver_.start).count();
    const std::vector<int> shown = driver_.showRange(seekedAt + 0.5 / FPS, seekedAt + 1.6);
    // Down from frame 19 to 0, then forward again (the reflection shows frame 0 twice).
    EXPECT_EQ(shown, concat({descending(19, 0), ascending(0, 19)}));
}

TEST_P(VideoPlayerPlayback, SwitchingLoopModeWhilePlaying) {
    player().setLoopMode(LoopMode::LOOP);
    player().play();
    std::vector<int> shown = driver_.showRange(0.5 / FPS, 1.0);
    EXPECT_EQ(shown, ascending(0, 24));

    player().setLoopMode(LoopMode::PING_PONG);  // forward, at 0.98 s (the last step)
    shown = driver_.showRange(1.0 + 0.5 / FPS, 2.8);
    EXPECT_EQ(shown, concat({ascending(25, FRAMES - 1), descending(FRAMES - 1, 30)}));

    player().setLoopMode(LoopMode::LOOP);  // on the reverse leg, at 1.22 s (the last step)
    shown = driver_.showRange(2.8 + 0.5 / FPS, 4.0);
    EXPECT_EQ(shown, concat({ascending(31, FRAMES - 1), ascending(0, 10)}));

    player().setLoopMode(LoopMode::ONCE);
    shown = driver_.showRange(4.0 + 0.5 / FPS, 6.0);
    EXPECT_EQ(shown.back(), FRAMES - 1);
    EXPECT_TRUE(driver_.waitFor([&] { return player().getState().isFinished(); }));
}

INSTANTIATE_TEST_SUITE_P(Containers, VideoPlayerPlayback,
                         testing::Values(ClipSpec{".mp4", 12},
                                         ClipSpec{".mkv", 12},
                                         ClipSpec{".ts", 12},
                                         ClipSpec{".mp4", 300}),  // the whole clip is one GOP
                         clipName);

// --- Single-container behaviour ---

class VideoPlayerMp4 : public testing::Test {
protected:
    void SetUp() override {
        path_ = clipPath("clip", ".mp4");
        if (!testclips::writeMpeg2Clip(path_, 12, FRAMES, FPS, START_OFFSET_FRAMES)) {
            GTEST_SKIP() << "FFmpeg cannot write " << path_;
        }
        driver_.start = now_;
        ASSERT_TRUE(driver_.player.load(path_));
    }

    void TearDown() override {
        driver_.player.unload();
        std::error_code ec;
        fs::remove(path_, ec);
    }

    VideoPlayer& player() { return driver_.player; }

    fs::path path_;
    PlaybackTimePoint now_ = PlaybackTimePoint{} + std::chrono::hours(2);
    Driver driver_{now_};
};

TEST_F(VideoPlayerMp4, OncePlaysToTheLastFrameAndFinishes) {
    int ended = 0;
    player().setEndCallback([&] { ++ended; });
    player().setLoopMode(LoopMode::ONCE);
    player().play();
    const std::vector<int> shown = driver_.showRange(0.5 / FPS, 2.6);
    EXPECT_EQ(shown, concat({ascending(0, FRAMES - 1), std::vector<int>(15, FRAMES - 1)}));
    // Finished once the decoder has reported the end of the stream.
    EXPECT_TRUE(driver_.waitFor([&] { return player().getState().isFinished(); }));
    EXPECT_EQ(ended, 1);
    EXPECT_NEAR(player().getState().position, DURATION, 1e-6);

    player().play();  // from the end (at 2.58 s, the last step): starts over
    EXPECT_EQ(driver_.showAt(2.58 + 0.5 / FPS), 0);
    EXPECT_EQ(driver_.showAt(2.58 + 3.5 / FPS), 3);
}

TEST_F(VideoPlayerMp4, PauseHoldsTheFrameAndResumeContinues) {
    player().play();
    driver_.showRange(0.5 / FPS, 0.5);  // frames 0..11, the last at 0.46 s
    player().pause();
    EXPECT_EQ(driver_.showAt(10.0), 11);  // 10 s later, still the frame at 0.46 s
    EXPECT_NEAR(player().getState().position, 0.46, 1e-6);
    player().play();  // resumed at 10 s, 0.46 s into the clip (mid frame 11)
    const std::vector<int> shown = driver_.showRange(10.0 + 1.0 / FPS, 10.42);
    EXPECT_EQ(shown, ascending(12, 21));
}

TEST_F(VideoPlayerMp4, FourTimesSpeedFollowsTheClockAcrossWraps) {
    player().setSpeed(4.0f);
    player().play();
    // Stepping one content frame of real time shows every 4th frame, wrapping each
    // 0.5 s. Steps sit an eighth of a frame in, so the clock is mid-frame at 4x.
    for (int i = 0; i < 37; ++i) {
        const int shown = driver_.showAt((i + 0.125) / FPS);
        ASSERT_EQ(shown, (4 * i) % FRAMES) << "step " << i;
    }
}

TEST_F(VideoPlayerMp4, StallResyncsToTheClock) {
    player().play();
    driver_.showRange(0.5 / FPS, 0.3);
    // The app froze for 30.5 s: the next frame is the one at 31 s mod 2 s, found
    // without decoding the 15 passes in between.
    EXPECT_EQ(driver_.showAt(31.02), 25);
    EXPECT_EQ(driver_.showAt(31.02 + 1.0 / FPS), 26);
}

TEST_F(VideoPlayerMp4, OversizedGopFallsBackToLoopWithAReason) {
    player().setReverseCacheBytes(16 * 1024);  // smaller than one 13-frame group
    player().setLoopMode(LoopMode::PING_PONG);
    EXPECT_EQ(player().getLoopMode(), LoopMode::PING_PONG);  // what the user chose stays
    EXPECT_NE(player().getLoopModeNotice().find("reverse cache"), std::string::npos)
        << player().getLoopModeNotice();
    player().play();
    const std::vector<int> shown = driver_.showRange(0.5 / FPS, 2 * DURATION);
    EXPECT_EQ(shown, concat({ascending(0, FRAMES - 1), ascending(0, FRAMES - 1)}));

    player().setLoopMode(LoopMode::LOOP);  // choosing another mode clears the notice
    EXPECT_TRUE(player().getLoopModeNotice().empty());
}

TEST_F(VideoPlayerMp4, ReverseCacheStaysWithinItsBudget) {
    constexpr size_t BUDGET = 160 * 1024;  // room for one 13-frame group, not the clip
    player().setReverseCacheBytes(BUDGET);
    player().setLoopMode(LoopMode::PING_PONG);
    ASSERT_TRUE(player().getLoopModeNotice().empty()) << player().getLoopModeNotice();
    player().play();
    const std::vector<int> shown = driver_.showRange(0.5 / FPS, 2 * DURATION);
    EXPECT_EQ(shown, concat({ascending(0, FRAMES - 1), descending(FRAMES - 1, 0)}));
    EXPECT_GT(player().getReverseCachePeakBytes(), 0u);
    EXPECT_LE(player().getReverseCachePeakBytes(), BUDGET);
    std::cout << "[ reverse cache peak " << player().getReverseCachePeakBytes() << " of "
              << BUDGET << " bytes ]\n";
}

// --- Timeline lock ---

TEST(VideoPlayerTimelineLock, TwoClipsOfDifferentLengthsStayPhaseExactThroughAnHour) {
    constexpr int SHORT_FRAMES = 30;  // 1.2 s against the 2 s clip
    const fs::path longPath = clipPath("long", ".mp4");
    const fs::path shortPath = clipPath("short", ".mp4");
    if (!testclips::writeMpeg2Clip(longPath, 12, FRAMES, FPS, 0) ||
        !testclips::writeMpeg2Clip(shortPath, 12, SHORT_FRAMES, FPS, 0)) {
        GTEST_SKIP() << "FFmpeg cannot write test clips";
    }
    PlaybackTimePoint now = PlaybackTimePoint{} + std::chrono::hours(3);
    PlaybackClock master;
    master.play(now);

    Driver a(now), b(now, SHORT_FRAMES);
    a.start = b.start = now;
    ASSERT_TRUE(a.player.load(longPath));
    ASSERT_TRUE(b.player.load(shortPath));
    b.player.setLoopMode(LoopMode::PING_PONG);
    a.player.setTimelineLocked(true, master);
    b.player.setTimelineLocked(true, master);
    EXPECT_EQ(a.player.getState().state, PlaybackState::PLAYING);

    auto check = [&](double t) {
        const int shownA = a.showAt(t, &master);
        const int shownB = b.showAt(t, &master);
        const double m = master.timeline(now);
        EXPECT_EQ(shownA, static_cast<int>(std::floor(mapTimeline(m, DURATION, LoopMode::LOOP).position * FPS + 1e-9)))
            << "t=" << t;
        const double bPos = mapTimeline(m, SHORT_FRAMES / double(FPS), LoopMode::PING_PONG).position;
        EXPECT_EQ(shownB, std::min(SHORT_FRAMES - 1, static_cast<int>(std::floor(bPos * FPS + 1e-9))))
            << "t=" << t;
    };
    for (double t = 0.5 / FPS; t < 3.0; t += 1.0 / FPS) check(t);

    // An hour later (seen as a stall) both are still exactly where master time puts them.
    for (double t = 3600.0 + 0.5 / FPS; t < 3602.0; t += 1.0 / FPS) check(t);
    EXPECT_DOUBLE_EQ(a.player.getClock().timeline(now), master.timeline(now));
    EXPECT_DOUBLE_EQ(b.player.getClock().timeline(now), master.timeline(now));

    // Pausing the master freezes both; restarting it brings both back to frame 0.
    master.pause(now);
    const double pausedAt = std::chrono::duration<double>(now - a.start).count();
    const int frozenA = a.showAt(pausedAt + 5.0, &master);
    EXPECT_EQ(a.showAt(pausedAt + 9.0, &master), frozenA);
    master.seek(0.0, now);
    master.play(now);
    EXPECT_EQ(a.showAt(pausedAt + 9.0 + 0.5 / FPS, &master), 0);
    EXPECT_EQ(b.showAt(pausedAt + 9.0 + 0.5 / FPS, &master), 0);

    a.player.unload();
    b.player.unload();
    std::error_code ec;
    fs::remove(longPath, ec);
    fs::remove(shortPath, ec);
}

TEST(VideoPlayerTimelineLock, PausedWhileUnseenResumesAtMasterTime) {
    const fs::path path = clipPath("clip", ".mp4");
    if (!testclips::writeMpeg2Clip(path, 12, FRAMES, FPS, 0)) GTEST_SKIP() << "no encoder";
    PlaybackTimePoint now = PlaybackTimePoint{} + std::chrono::hours(3);
    PlaybackClock master;
    master.play(now);
    Driver d(now);
    d.start = now;
    ASSERT_TRUE(d.player.load(path));
    d.player.setTimelineLocked(true, master);
    d.showRange(0.5 / FPS, 0.4, &master);

    d.player.pause();  // what releaseUnusedSources does off screen
    const int held = frameIndex(d.player.getCurrentFrame());
    now = d.start + std::chrono::milliseconds(2500);
    d.player.followTimeline(master);
    d.player.update(0.0);
    EXPECT_NEAR(d.player.getState().position, 0.5, 1e-9);    // master time kept while hidden
    EXPECT_EQ(frameIndex(d.player.getCurrentFrame()), held);  // but no frames taken
    d.player.play();  // back on screen
    EXPECT_EQ(d.showAt(3.0 + 0.5 / FPS, &master), 25);
    EXPECT_EQ(d.showAt(3.0 + 1.5 / FPS, &master), 26);

    d.player.setTimelineLocked(false, master);  // unlocked: carries on from there
    EXPECT_EQ(d.showAt(3.0 + 2.5 / FPS, &master), 27);
    d.player.unload();
    std::error_code ec;
    fs::remove(path, ec);
}

// --- Decoder: a reverse leg whose group of pictures exceeds the cache ---

// The decoder enforces the budget itself while playing (players also check a file's
// index up front): it stops at the reverse leg, says why, and stays within budget.
TEST(VideoDecoderReverse, GroupTooLargeForTheCacheIsReportedWhilePlaying) {
    const fs::path path = clipPath("clip", ".mp4");
    if (!testclips::writeMpeg2Clip(path, 300, FRAMES, FPS, 0)) GTEST_SKIP() << "no encoder";
    VideoDecoder decoder;
    ASSERT_TRUE(decoder.open(path));
    DecodePlan plan;
    plan.duration = decoder.getInfo().duration;
    plan.loopMode = LoopMode::PING_PONG;
    plan.reverseCacheBytes = 32 * 1024;
    decoder.startPlayback(plan, 4);

    std::vector<int> frames;
    const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
    while (decoder.getReverseError().empty() && std::chrono::steady_clock::now() < deadline) {
        VideoFrame f = decoder.getBufferedFrame();
        if (f.isValid()) frames.push_back(frameIndex(f));
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_NE(decoder.getReverseError().find("reverse cache"), std::string::npos)
        << decoder.getReverseError();
    for (VideoFrame f = decoder.getBufferedFrame(); f.isValid(); f = decoder.getBufferedFrame()) {
        frames.push_back(frameIndex(f));
    }
    EXPECT_EQ(frames, ascending(0, FRAMES - 1));  // the forward leg, then nothing
    EXPECT_LE(decoder.getReverseCachePeakBytes(), plan.reverseCacheBytes);
    decoder.close();
    std::error_code ec;
    fs::remove(path, ec);
}

// --- The loop seam (TD-30) ---

// Loop feeds the start of the clip to the decoder straight after its end, so the next
// pass's first frames are queued before the seam and the codec is never restarted
// there (a restart set up a new GPU decoder per stream and delayed every clip that
// wrapped at once). Nothing is consumed while the queue fills, so what is queued does
// not depend on timing: the frames due just before and after the seam, in order.
class VideoDecoderLoopSeam : public testing::TestWithParam<ClipSpec> {
protected:
    void SetUp() override {
        path_ = clipPath("clip", GetParam().extension);
        if (!testclips::writeMpeg2Clip(path_, GetParam().gopSize, FRAMES, FPS, START_OFFSET_FRAMES)) {
            GTEST_SKIP() << "FFmpeg cannot write " << path_;
        }
        ASSERT_TRUE(decoder_.open(path_));
    }

    void TearDown() override {
        decoder_.close();
        std::error_code ec;
        fs::remove(path_, ec);
    }

    // Wait (real time, bounded) until `count` frames are queued.
    bool waitQueued(size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
        while (decoder_.getBufferedFrameCount() < count) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    fs::path path_;
    VideoDecoder decoder_;
};

TEST_P(VideoDecoderLoopSeam, NextPassIsQueuedBeforeTheSeamWithoutARestart) {
    constexpr int BEFORE = 3;  // start three frames before the seam
    constexpr size_t QUEUE = 8;
    DecodePlan plan;
    plan.duration = DURATION;
    plan.loopMode = LoopMode::LOOP;
    plan.timeline = DURATION - double(BEFORE) / FPS;
    decoder_.startPlayback(plan, QUEUE);
    // The positioning seek (and any seek back it needed) is done once a frame is out.
    ASSERT_TRUE(waitQueued(1));
    const uint64_t restarts = decoder_.decoderRestarts();

    ASSERT_TRUE(waitQueued(QUEUE));
    EXPECT_EQ(decoder_.decoderRestarts(), restarts) << "the codec restarted at the seam";
    for (size_t i = 0; i < QUEUE; ++i) {
        const VideoFrame frame = decoder_.getBufferedFrame();
        const int expected = (FRAMES - BEFORE + static_cast<int>(i)) % FRAMES;
        ASSERT_EQ(frameIndex(frame), expected) << "queued frame " << i;
        EXPECT_NEAR(frame.due, plan.timeline + double(i) / FPS, DUE_TOLERANCE) << "queued frame " << i;
    }

    // Two more seams: every frame once, each due one frame after the last, no restart.
    double lastDue = plan.timeline + double(QUEUE - 1) / FPS;
    int lastIndex = (FRAMES - BEFORE + static_cast<int>(QUEUE) - 1) % FRAMES;
    for (int n = 0; n < 2 * FRAMES; ++n) {
        ASSERT_TRUE(waitQueued(1)) << "after frame " << lastIndex;
        const VideoFrame frame = decoder_.getBufferedFrame();
        ASSERT_EQ(frameIndex(frame), (lastIndex + 1) % FRAMES) << "step " << n;
        ASSERT_NEAR(frame.due, lastDue + 1.0 / FPS, DUE_TOLERANCE) << "step " << n;
        lastIndex = frameIndex(frame);
        lastDue = frame.due;
    }
    EXPECT_EQ(decoder_.decoderRestarts(), restarts);
    EXPECT_NEAR(decoder_.getObservedEnd(), DURATION, DUE_TOLERANCE);
}

INSTANTIATE_TEST_SUITE_P(Containers, VideoDecoderLoopSeam,
                         testing::Values(ClipSpec{".mp4", 12},
                                         ClipSpec{".mkv", 12},
                                         ClipSpec{".ts", 12},
                                         ClipSpec{".mp4", 300}),
                         clipName);
