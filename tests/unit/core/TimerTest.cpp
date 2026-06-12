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
    timer.reset();

    double elapsed = timer.elapsedSeconds();
    EXPECT_LT(elapsed, 0.01);
}

TEST(FrameTimerTest, DeltaTimeCalculation) {
    FrameTimer frameTimer;

    frameTimer.update();
    std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    frameTimer.update();

    float delta = frameTimer.getDeltaTime();
    EXPECT_GT(delta, 0.0f);
    EXPECT_LT(delta, 1.0f); // Should be much less than 1 second
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
