// Frame pacing (QM-088): the shown frame follows the playback clock, not the render
// loop. Simulates the VideoPlayer / HapFileSource update — advance the clock by
// deltaTime * speed, then popDueFrames() — over a queue of decoded frames.

#include <gtest/gtest.h>

#include "video/FrameSelection.hpp"

#include <deque>
#include <vector>

using namespace quadmapper;

namespace {

// Plays `updates` render-loop ticks at updateHz over content at contentFps and
// returns the index of the frame on screen after each tick (-1 = nothing yet).
std::vector<int> simulate(double contentFps, double updateHz, double speed, int updates) {
    std::deque<int> queue;  // frame indices; pts = index / fps
    for (int i = 0; i < 1000; ++i) queue.push_back(i);

    std::vector<int> shown;
    double clock = 0.0;
    int current = -1;
    for (int u = 0; u < updates; ++u) {
        clock += (1.0 / updateHz) * speed;
        popDueFrames(
            clock, current >= 0,
            [&](double& pts) {
                if (queue.empty()) return false;
                pts = queue.front() / contentFps;
                return true;
            },
            [&]() {
                current = queue.front();
                queue.pop_front();
            });
        shown.push_back(current);
    }
    return shown;
}

}  // namespace

TEST(FrameSelection, ContentAt30FpsOn60HzLoopAdvancesOneFramePerTwoUpdates) {
    const std::vector<int> shown = simulate(30.0, 60.0, 1.0, 60);
    // One second of updates shows exactly one second of content, each frame twice.
    for (int u = 0; u < 60; ++u) {
        EXPECT_EQ(shown[u], (u + 1) / 2) << "update " << u;
    }
}

TEST(FrameSelection, HalfSpeedHalvesTheFrameRate) {
    const std::vector<int> shown = simulate(30.0, 60.0, 0.5, 120);
    for (int u = 0; u < 120; ++u) {
        EXPECT_EQ(shown[u], (u + 1) / 4) << "update " << u;
    }
    EXPECT_EQ(shown.back(), 30);  // two seconds of updates = one second of content
}

TEST(FrameSelection, DoubleSpeedSkipsFrames) {
    // 30 fps content, 30 Hz loop, 2x: every update is two frames of content, so the
    // frame between them is dropped rather than shown late.
    const std::vector<int> shown = simulate(30.0, 30.0, 2.0, 30);
    for (int u = 0; u < 30; ++u) {
        EXPECT_EQ(shown[u], 2 * (u + 1)) << "update " << u;
    }
}

TEST(FrameSelection, NeverShowsAFrameAheadOfTheClock) {
    const double fps = 24.0;
    std::deque<double> queue;
    for (int i = 0; i < 100; ++i) queue.push_back(i / fps);

    double clock = 0.0;
    double shownPts = -1.0;
    for (int u = 0; u < 200; ++u) {
        clock += 1.0 / 144.0;
        popDueFrames(
            clock, shownPts >= 0.0,
            [&](double& pts) {
                if (queue.empty()) return false;
                pts = queue.front();
                return true;
            },
            [&]() {
                shownPts = queue.front();
                queue.pop_front();
            });
        EXPECT_LE(shownPts, clock + FRAME_DUE_EPSILON) << "update " << u;
    }
}

TEST(FrameSelection, FirstFrameShowsAtOnceEvenIfItsPtsIsLate) {
    // Nothing on screen yet: the first frame is taken even though its pts (0.5 s)
    // is ahead of the clock, so playback does not start black.
    std::deque<double> queue{0.5, 0.6};
    int popped = 0;
    const size_t n = popDueFrames(
        0.0, /*haveFrame=*/false,
        [&](double& pts) {
            if (queue.empty()) return false;
            pts = queue.front();
            return true;
        },
        [&]() {
            queue.pop_front();
            ++popped;
        });
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(popped, 1);
    EXPECT_EQ(queue.size(), 1u);  // 0.6 stays queued: ahead of the clock
}

TEST(FrameSelection, IsFrameDueRule) {
    EXPECT_TRUE(isFrameDue(1.0, 1.0, true));
    EXPECT_TRUE(isFrameDue(0.5, 1.0, true));
    EXPECT_FALSE(isFrameDue(1.01, 1.0, true));
    EXPECT_TRUE(isFrameDue(5.0, 0.0, false));
}
