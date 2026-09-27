// HapFileSource on the playback clock (ADR-050), demuxing a HAP clip written at test
// time, with injected clock time. HAP frames are all keyframes, so Ping-Pong's
// reverse leg must show every frame, exactly, in descending order.

#include <gtest/gtest.h>

#include "core/FileSystem.hpp"
#include "source/HapFileSource.hpp"
#include "video/FrameSelection.hpp"
#include "video/PlaybackClock.hpp"
#include "../video/TestClips.hpp"

#include <process.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace quadmapper;
namespace fs = std::filesystem;

namespace {

constexpr int FPS = 25;
constexpr int FRAMES = 40;  // 1.6 s
constexpr double DURATION = double(FRAMES) / FPS;
const auto WAIT_LIMIT = std::chrono::seconds(10);

std::vector<int> ascending(int from, int to) {
    std::vector<int> v;
    for (int i = from; i <= to; ++i) v.push_back(i);
    return v;
}

std::vector<int> descending(int from, int to) {
    std::vector<int> v;
    for (int i = from; i >= to; --i) v.push_back(i);
    return v;
}

std::vector<int> concat(std::initializer_list<std::vector<int>> parts) {
    std::vector<int> v;
    for (const auto& p : parts) v.insert(v.end(), p.begin(), p.end());
    return v;
}

// The frame the clock shows at `now` (a frame counts as due FRAME_DUE_EPSILON early).
int expectedFrame(const PlaybackClock& clock, PlaybackTimePoint now) {
    const ClipPosition clip = clock.sample(now);
    const int frame = clip.direction > 0
        ? static_cast<int>(std::floor((clip.position + FRAME_DUE_EPSILON) * FPS))
        : static_cast<int>(std::ceil((clip.position - FRAME_DUE_EPSILON) * FPS)) - 1;
    return std::clamp(frame, 0, FRAMES - 1);
}

class HapPlayback : public testing::Test {
protected:
    void SetUp() override {
        std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
        // The process id keeps concurrent test runs (other build trees) off each other's file.
        path_ = fs::temp_directory_path() / ("qm_hap_" + name + "_" + std::to_string(_getpid()) + ".mov");
        if (!testclips::writeHapClip(path_, FRAMES, FPS)) GTEST_SKIP() << "FFmpeg cannot write HAP";
        source_ = std::make_unique<HapFileSource>(path_);
        source_->setTimeSource([this] { return now_; });
        ASSERT_TRUE(source_->initialize());
        ASSERT_NEAR(source_->getDuration(), DURATION, 1e-6);
    }

    void TearDown() override {
        source_.reset();
        std::error_code ec;
        fs::remove(path_, ec);
    }

    int shownFrame() const {
        const FrameData* frame = source_->getCurrentFrame();
        return frame ? static_cast<int>(std::lround(frame->timestamp / 1e6 * FPS)) : -1;
    }

    // Set the clock to `seconds` after the start and wait until the source shows the
    // frame the clock schedules there.
    int showAt(double seconds, const PlaybackClock* master = nullptr) {
        now_ = start_ + std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::duration<double>(seconds));
        const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
        while (std::chrono::steady_clock::now() < deadline) {
            if (master) source_->followTimeline(*master);
            source_->update(0.0);
            source_->markFrameConsumed();
            const FrameData* frame = source_->getCurrentFrame();
            if (frame && frame->isCompressed() && shownFrame() == expectedFrame(source_->getClock(), now_)) {
                return shownFrame();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ADD_FAILURE() << "no frame for t=" << seconds << " (clip position "
                      << source_->getClock().position(now_) << ", showing " << shownFrame() << ")";
        return -1;
    }

    std::vector<int> showRange(double from, double to, const PlaybackClock* master = nullptr) {
        std::vector<int> shown;
        for (double t = from; t < to - 1e-9; t += 1.0 / FPS) {
            shown.push_back(showAt(t, master));
            if (shown.back() < 0) break;
        }
        return shown;
    }

    fs::path path_;
    PlaybackTimePoint now_ = PlaybackTimePoint{} + std::chrono::hours(4);
    PlaybackTimePoint start_ = now_;
    std::unique_ptr<HapFileSource> source_;
};

}  // namespace

// The HAP demuxer opens a clip in a folder named outside the ANSI code page.
TEST(HapFileSourcePaths, OpensClipInNonAsciiFolder) {
    const fs::path folder = fs::temp_directory_path() /
                            pathFromUtf8(u8"qm_hap_\u00fc \u65e5\u672c \u0416_" + std::to_string(_getpid()));
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path path = folder / pathFromUtf8(u8"\u0416.mov");
    if (!testclips::writeHapClip(path, FRAMES, FPS)) {
        fs::remove_all(folder, ec);
        GTEST_SKIP() << "FFmpeg cannot write HAP";
    }
    bool initialized = false;
    std::string name;
    {
        HapFileSource source(path);
        initialized = source.initialize();
        name = source.getName();
    }
    fs::remove_all(folder, ec);
    EXPECT_TRUE(initialized);
    EXPECT_EQ(name, u8"\u0416.mov");
}

TEST_F(HapPlayback, LoopShowsTheFirstFramesAfterEachWrap) {
    source_->setLoopMode(LoopMode::LOOP);
    source_->play();
    const std::vector<int> shown = showRange(0.5 / FPS, 3 * DURATION);
    EXPECT_EQ(shown, concat({ascending(0, FRAMES - 1), ascending(0, FRAMES - 1), ascending(0, FRAMES - 1)}));
    const double elapsed = std::chrono::duration<double>(now_ - start_).count();
    EXPECT_NEAR(source_->getClock().timeline(now_), elapsed, 1e-9);  // never restarted
    EXPECT_TRUE(source_->getErrorMessage().empty());
}

// TD-31: frame buffers are reused, so playing allocates and frees nothing per frame,
// but never while anyone still holds one (a frame shown, queued, or being uploaded).
// The frame after the one shown is offered to the upload early (nextCompressed).
TEST_F(HapPlayback, FrameBuffersAreReusedButNeverWhileHeld) {
    source_->setLoopMode(LoopMode::LOOP);
    source_->play();
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> held;  // the first pass, kept
    std::vector<const void*> seen;                                   // every buffer shown
    std::shared_ptr<const std::vector<uint8_t>> offered;             // the previous frame's hint
    int offeredMatches = 0;
    for (int step = 0; step < 3 * FRAMES; ++step) {
        ASSERT_GE(showAt((step + 0.5) / FPS), 0) << "step " << step;
        const FrameData* frame = source_->getCurrentFrame();
        ASSERT_TRUE(frame && frame->compressed);
        if (offered) {
            EXPECT_EQ(offered, frame->compressed) << "the hint was not the next frame, step " << step;
            ++offeredMatches;
        }
        offered = frame->nextCompressed;
        seen.push_back(frame->compressed.get());
        if (step < FRAMES) held.push_back(frame->compressed);
        if (step == FRAMES - 1) held.clear();  // let the first pass go
    }
    // Held buffers are never handed out again while held.
    std::vector<const void*> firstPass(seen.begin(), seen.begin() + FRAMES);
    std::sort(firstPass.begin(), firstPass.end());
    EXPECT_EQ(std::unique(firstPass.begin(), firstPass.end()), firstPass.end());
    // Once let go, the last pass reuses a bounded set of buffers.
    std::vector<const void*> later(seen.begin() + 2 * FRAMES, seen.end());
    std::sort(later.begin(), later.end());
    later.erase(std::unique(later.begin(), later.end()), later.end());
    EXPECT_LE(later.size(), 12u);  // HapFileSource's pool: queue (8) + 4
    EXPECT_GT(offeredMatches, 0);
}

TEST_F(HapPlayback, PingPongReverseLegIsFrameExact) {
    source_->setLoopMode(LoopMode::PING_PONG);
    EXPECT_TRUE(source_->getLoopModeNotice().empty());
    source_->play();
    const std::vector<int> shown = showRange(0.5 / FPS, 3 * DURATION);
    EXPECT_EQ(shown, concat({ascending(0, FRAMES - 1), descending(FRAMES - 1, 0), ascending(0, FRAMES - 1)}));
}

TEST_F(HapPlayback, SeekDuringTheReverseLegKeepsReversing) {
    source_->setLoopMode(LoopMode::PING_PONG);
    source_->play();
    showRange(0.5 / FPS, DURATION + 0.3);  // last step at 1.86 s: reverse, 1.34 s in
    ASSERT_EQ(source_->getClock().direction(now_), -1);
    source_->seek(0.8);  // the start of frame 20
    const double seekedAt = std::chrono::duration<double>(now_ - start_).count();
    const std::vector<int> shown = showRange(seekedAt + 0.5 / FPS, seekedAt + 1.6);
    EXPECT_EQ(shown, concat({descending(19, 0), ascending(0, 19)}));
}

TEST_F(HapPlayback, SwitchingLoopModeWhilePlaying) {
    source_->play();
    EXPECT_EQ(showRange(0.5 / FPS, 1.0), ascending(0, 24));
    source_->setLoopMode(LoopMode::PING_PONG);  // forward at 0.98 s
    EXPECT_EQ(showRange(1.0 + 0.5 / FPS, 2.0),
              concat({ascending(25, FRAMES - 1), descending(FRAMES - 1, 30)}));
    source_->setLoopMode(LoopMode::LOOP);  // reverse at 1.22 s -> forward from there
    EXPECT_EQ(showRange(2.0 + 0.5 / FPS, 2.8), concat({ascending(31, FRAMES - 1), ascending(0, 10)}));
}

TEST_F(HapPlayback, OncePlaysToTheEndAndFinishes) {
    source_->setLoopMode(LoopMode::ONCE);
    source_->play();
    const std::vector<int> shown = showRange(0.5 / FPS, DURATION + 0.4);
    EXPECT_EQ(shown, concat({ascending(0, FRAMES - 1), std::vector<int>(10, FRAMES - 1)}));
    const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
    while (source_->getPlaybackState() != PlaybackState::FINISHED &&
           std::chrono::steady_clock::now() < deadline) {
        source_->update(0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(source_->getPlaybackState(), PlaybackState::FINISHED);
    EXPECT_NEAR(source_->getCurrentTime(), DURATION, 1e-6);
}

TEST_F(HapPlayback, LockedToTheMasterAndPausedWhileUnseen) {
    PlaybackClock master;
    master.play(now_);
    source_->setLoopMode(LoopMode::PING_PONG);
    source_->setTimelineLocked(true, master);
    EXPECT_TRUE(source_->isTimelineLocked());
    for (double t = 0.5 / FPS; t < 4.0; t += 1.0 / FPS) {
        const int shown = showAt(t, &master);
        const double position = mapTimeline(master.timeline(now_), DURATION, LoopMode::PING_PONG).position;
        ASSERT_EQ(shown, std::min(FRAMES - 1, static_cast<int>(std::floor(position * FPS + 1e-9))))
            << "t=" << t;
    }
    // Own transport is ignored while locked.
    source_->seek(0.2);
    source_->setSpeed(3.0f);
    EXPECT_DOUBLE_EQ(source_->getClock().timeline(now_), master.timeline(now_));

    source_->pause();  // off screen: master time kept, no frames taken
    const int held = shownFrame();
    now_ = start_ + std::chrono::milliseconds(6100);
    source_->followTimeline(master);
    source_->update(0.0);
    EXPECT_EQ(shownFrame(), held);
    source_->play();  // back on screen at master time: 6.1 s is reverse leg 3, clip position 0.3 s
    EXPECT_EQ(showAt(6.1, &master), 7);

    // An hour on (seen as a stall), still phase-exact with the master.
    EXPECT_EQ(showAt(3600.0 + 0.5 / FPS, &master), 0);   // leg 2250 (forward), 0.02 s in
    EXPECT_EQ(showAt(3600.0 + 10.5 / FPS, &master), 10);
}
