#include <gtest/gtest.h>
#include "core/PerformanceMonitor.hpp"
#include <thread>
#include <chrono>

namespace quadmapper {
namespace test {

class PerformanceMonitorTest : public ::testing::Test {
protected:
    void SetUp() override {
        monitor_ = std::make_unique<PerformanceMonitor>();
    }

    std::unique_ptr<PerformanceMonitor> monitor_;
};

TEST_F(PerformanceMonitorTest, InitialStateHasNoFrameHistory) {
    EXPECT_TRUE(monitor_->getFrameTimeHistory().empty());
    EXPECT_DOUBLE_EQ(monitor_->getAverageFPS(), 0.0);
}

TEST_F(PerformanceMonitorTest, FrameTimingRecorded) {
    monitor_->beginFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    monitor_->endFrame();

    const auto& history = monitor_->getFrameTimeHistory();
    EXPECT_EQ(history.size(), 1);
    EXPECT_GT(history.back(), 9.0);  // Should be at least 9ms
}

TEST_F(PerformanceMonitorTest, AverageFPSCalculated) {
    // Simulate a few frames at ~100 fps (10ms each)
    for (int i = 0; i < 10; ++i) {
        monitor_->beginFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        monitor_->endFrame();
    }

    double fps = monitor_->getAverageFPS();
    EXPECT_GT(fps, 50.0);  // Should be roughly 100 fps, but allow for variance
    EXPECT_LT(fps, 150.0);
}

TEST_F(PerformanceMonitorTest, MinMaxFrameTime) {
    monitor_->beginFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    monitor_->endFrame();

    monitor_->beginFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    monitor_->endFrame();

    monitor_->beginFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    monitor_->endFrame();

    // Using wider tolerance due to OS scheduling variance
    EXPECT_GT(monitor_->getMinFrameTime(), 2.0);
    EXPECT_LT(monitor_->getMinFrameTime(), 15.0);
    EXPECT_GT(monitor_->getMaxFrameTime(), 15.0);
}

TEST_F(PerformanceMonitorTest, DrawCallsRecorded) {
    monitor_->beginFrame();
    monitor_->recordDrawCalls(5);
    monitor_->recordDrawCalls(3);
    monitor_->endFrame();

    EXPECT_EQ(monitor_->getCurrentStats().drawCalls, 8);
}

TEST_F(PerformanceMonitorTest, TrianglesRecorded) {
    monitor_->beginFrame();
    monitor_->recordTriangles(100);
    monitor_->recordTriangles(50);
    monitor_->endFrame();

    EXPECT_EQ(monitor_->getCurrentStats().triangles, 150);
}

TEST_F(PerformanceMonitorTest, TextureMemoryRecorded) {
    monitor_->recordTextureMemory(5 * 1024 * 1024);  // 5 MB

    EXPECT_EQ(monitor_->getCurrentStats().textureMemoryMB, 5);
}

TEST_F(PerformanceMonitorTest, ResetClearsHistory) {
    monitor_->beginFrame();
    monitor_->recordDrawCalls(10);
    monitor_->endFrame();

    monitor_->reset();

    EXPECT_TRUE(monitor_->getFrameTimeHistory().empty());
    EXPECT_EQ(monitor_->getCurrentStats().drawCalls, 0);
}

TEST_F(PerformanceMonitorTest, OverlaySettingsAccessible) {
    auto& settings = monitor_->getOverlaySettings();

    EXPECT_TRUE(settings.showFPS);  // Default value
    settings.showGraph = true;

    EXPECT_TRUE(monitor_->getOverlaySettings().showGraph);
}

TEST_F(PerformanceMonitorTest, HistoryLimitedToMaxSize) {
    // Fill beyond history size
    for (size_t i = 0; i < PerformanceMonitor::HISTORY_SIZE + 10; ++i) {
        monitor_->beginFrame();
        monitor_->endFrame();
    }

    EXPECT_EQ(monitor_->getFrameTimeHistory().size(), PerformanceMonitor::HISTORY_SIZE);
}

// Test ScopedTimer
TEST(ScopedTimerTest, MeasuresElapsedTime) {
    double elapsed;
    {
        ScopedTimer timer("test");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        elapsed = timer.getElapsedMs();
    }

    EXPECT_GT(elapsed, 9.0);
}

// Test ScopedSection
TEST_F(PerformanceMonitorTest, SectionTimingRecorded) {
    monitor_->beginFrame();
    {
        monitor_->beginSection("render");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        monitor_->endSection("render");
    }
    monitor_->endFrame();

    EXPECT_GT(monitor_->getCurrentStats().renderTimeMs, 4.0);
}

} // namespace test
} // namespace quadmapper
