#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include "core/Timer.hpp"

namespace quadmapper {
namespace test {

TEST(TimerTest, InitiallyNotRunning) {
    Timer timer;
    EXPECT_FALSE(timer.isRunning());
}

TEST(TimerTest, StartMakesTimerRunning) {
    Timer timer;
    timer.start();
    EXPECT_TRUE(timer.isRunning());
}

TEST(TimerTest, StopMakesTimerNotRunning) {
    Timer timer;
    timer.start();
    timer.stop();
    EXPECT_FALSE(timer.isRunning());
}

TEST(TimerTest, ElapsedIncreases) {
    Timer timer;
    timer.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    double elapsed = timer.elapsedSeconds();
    EXPECT_GT(elapsed, 0.0);
}

TEST(TimerTest, ResetClearsElapsed) {
    Timer timer;
    timer.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // Bound the post-reset reading by the wall time around it rather than a fixed
    // 10 ms, which a preempted thread under ctest -j could exceed (TD-23).
    using Clock = std::chrono::high_resolution_clock;
    const auto before = Clock::now();
    timer.reset();
    const double elapsed = timer.elapsedSeconds();
    const double window = std::chrono::duration<double>(Clock::now() - before).count();

    EXPECT_GE(elapsed, 0.0);
    EXPECT_LE(elapsed, window);
    EXPECT_TRUE(timer.isRunning());
}

TEST(FrameTimerTest, DeltaTimeCalculation) {
    FrameTimer frameTimer;

    using Clock = std::chrono::high_resolution_clock;
    const auto before = Clock::now();
    frameTimer.update();
    std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    frameTimer.update();
    const double window = std::chrono::duration<double>(Clock::now() - before).count();

    // The delta covers the sleep and lies inside the wall time around both updates.
    const float delta = frameTimer.getDeltaTime();
    EXPECT_GE(delta, 0.015f);
    EXPECT_LE(delta, static_cast<float>(window) + 1e-6f);
}

TEST(FrameTimerTest, FPSCalculation) {
    FrameTimer frameTimer;

    // Simulate a few frames
    for (int i = 0; i < 10; ++i) {
        frameTimer.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    float fps = frameTimer.getFPS();
    EXPECT_GT(fps, 0.0f);
}

} // namespace test
} // namespace quadmapper
