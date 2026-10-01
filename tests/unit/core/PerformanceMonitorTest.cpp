#include <gtest/gtest.h>
#include "core/PerformanceMonitor.hpp"
#include "core/MainThreadTimings.hpp"
#include <thread>
#include <chrono>
#include <vector>

namespace quadmapper {
namespace test {

// Frame timing runs on a fake clock the test advances by hand, so the numbers are
// exact however loaded the machine is (TD-23: wall-clock sleeps flaked under ctest -j).
class PerformanceMonitorTest : public ::testing::Test {
protected:
    void SetUp() override {
        monitor_ = std::make_unique<PerformanceMonitor>([this] { return now_; });
    }

    void advanceMs(double ms) {
        now_ += std::chrono::duration_cast<PerformanceMonitor::Clock::duration>(
            std::chrono::duration<double, std::milli>(ms));
    }

    void frameOf(double ms) {
        monitor_->beginFrame();
        advanceMs(ms);
        monitor_->endFrame();
    }

    PerformanceMonitor::Clock::time_point now_{};
    std::unique_ptr<PerformanceMonitor> monitor_;
};

TEST_F(PerformanceMonitorTest, InitialStateHasNoFrameHistory) {
    EXPECT_TRUE(monitor_->getFrameTimeHistory().empty());
    EXPECT_DOUBLE_EQ(monitor_->getAverageFPS(), 0.0);
}

TEST_F(PerformanceMonitorTest, FrameTimingRecorded) {
    frameOf(10.0);

    const auto& history = monitor_->getFrameTimeHistory();
    ASSERT_EQ(history.size(), 1u);
    EXPECT_NEAR(history.back(), 10.0, 1e-6);
    EXPECT_NEAR(monitor_->getCurrentStats().frameTimeMs, 10.0, 1e-6);
}

TEST_F(PerformanceMonitorTest, AverageFPSCalculated) {
    for (int i = 0; i < 10; ++i) frameOf(10.0);  // 100 fps

    EXPECT_NEAR(monitor_->getAverageFrameTime(), 10.0, 1e-6);
    EXPECT_NEAR(monitor_->getAverageFPS(), 100.0, 1e-4);
}

TEST_F(PerformanceMonitorTest, AverageMixesFrameTimes) {
    frameOf(10.0);
    frameOf(30.0);

    EXPECT_NEAR(monitor_->getAverageFrameTime(), 20.0, 1e-6);
    EXPECT_NEAR(monitor_->getAverageFPS(), 50.0, 1e-4);
}

TEST_F(PerformanceMonitorTest, MinMaxFrameTime) {
    frameOf(5.0);
    frameOf(20.0);
    frameOf(10.0);

    EXPECT_NEAR(monitor_->getMinFrameTime(), 5.0, 1e-6);
    EXPECT_NEAR(monitor_->getMaxFrameTime(), 20.0, 1e-6);
}

TEST_F(PerformanceMonitorTest, DefaultMonitorMeasuresTheRealClock) {
    // The default time source is the real clock: a sleep can only lengthen a frame.
    PerformanceMonitor real;
    real.beginFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    real.endFrame();
    EXPECT_GE(real.getFrameTimeHistory().back(), 9.0);
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

TEST_F(PerformanceMonitorTest, SectionTimingRecorded) {
    monitor_->beginFrame();
    monitor_->beginSection("render");
    advanceMs(5.0);
    monitor_->endSection("render");
    monitor_->beginSection("ui");
    advanceMs(2.0);
    monitor_->endSection("ui");
    monitor_->endFrame();

    EXPECT_NEAR(monitor_->getCurrentStats().renderTimeMs, 5.0, 1e-6);
    EXPECT_NEAR(monitor_->getCurrentStats().uiTimeMs, 2.0, 1e-6);
    EXPECT_NEAR(monitor_->getCurrentStats().frameTimeMs, 7.0, 1e-6);
}

// The TD-35 main-thread timings record nothing until a harness marks the main thread
// (the app does only under --stress and --bench), and never on another thread, so a
// show takes no lock and keeps no samples. Marking is for the life of the process: the
// marking steps run last, in this one test.
TEST(MainThreadTimingsTest, NothingIsRecordedUntilTheMainThreadIsMarked) {
    MainThreadTimings::reset();
    { ScopedMainThreadTiming timing("unmarked"); }
    EXPECT_TRUE(MainThreadTimings::summarize().empty()) << "recorded with timings off";

    MainThreadTimings::markMainThread();
    std::thread([] { ScopedMainThreadTiming timing("worker"); }).join();
    { ScopedMainThreadTiming timing("marked"); }
    const std::vector<MainThreadTimings::Summary> recorded = MainThreadTimings::summarize();
    ASSERT_EQ(recorded.size(), 1u);
    EXPECT_EQ(recorded.front().op, "marked");
    EXPECT_EQ(recorded.front().count, 1u);
    MainThreadTimings::reset();
}

} // namespace test
} // namespace quadmapper
