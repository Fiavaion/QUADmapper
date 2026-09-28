#pragma once

#include <string>
#include <chrono>
#include <deque>
#include <functional>
#include <unordered_map>
#include <mutex>

namespace quadmapper {

// Simple high-precision timer for profiling
class ScopedTimer {
public:
    using Clock = std::chrono::high_resolution_clock;

    explicit ScopedTimer(const std::string& name);
    ~ScopedTimer();

    // Get elapsed time in milliseconds
    double getElapsedMs() const;

    // Disable copy
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    std::string name_;
    Clock::time_point startTime_;
};

// Frame timing statistics
struct FrameStats {
    double frameTimeMs = 0.0;       // Total frame time
    double renderTimeMs = 0.0;      // Time spent rendering
    double updateTimeMs = 0.0;      // Time spent updating logic
    double uiTimeMs = 0.0;          // Time spent on UI
    int drawCalls = 0;              // Number of draw calls
    int triangles = 0;              // Number of triangles rendered
    size_t textureMemoryMB = 0;     // Estimated texture memory usage
};

// Performance overlay settings
struct PerformanceOverlaySettings {
    bool showFPS = true;
    bool showFrameTime = true;
    bool showRenderStats = false;
    bool showMemoryStats = false;
    bool showGraph = false;
    float graphWidth = 200.0f;
    float graphHeight = 50.0f;
};

// PerformanceMonitor tracks frame timing and performance metrics
class PerformanceMonitor {
public:
    static constexpr size_t HISTORY_SIZE = 120;  // ~2 seconds at 60fps

    using Clock = std::chrono::high_resolution_clock;
    using TimeSource = std::function<Clock::time_point()>;

    // `now` is injectable so tests can drive frame timing deterministically; empty
    // means the real clock.
    explicit PerformanceMonitor(TimeSource now = {});
    ~PerformanceMonitor() = default;

    // Non-copyable
    PerformanceMonitor(const PerformanceMonitor&) = delete;
    PerformanceMonitor& operator=(const PerformanceMonitor&) = delete;

    // Frame timing
    void beginFrame();
    void endFrame();

    // Section timing
    void beginSection(const std::string& name);
    void endSection(const std::string& name);

    // Stats recording
    void recordDrawCalls(int count);
    void recordTriangles(int count);
    void recordTextureMemory(size_t bytes);

    // Current frame stats
    const FrameStats& getCurrentStats() const { return currentStats_; }

    // Averages (over history)
    double getAverageFPS() const;
    double getAverageFrameTime() const;
    double getMinFrameTime() const;
    double getMaxFrameTime() const;

    // Frame history for graphing
    const std::deque<double>& getFrameTimeHistory() const { return frameTimeHistory_; }

    // Overlay settings
    PerformanceOverlaySettings& getOverlaySettings() { return overlaySettings_; }
    const PerformanceOverlaySettings& getOverlaySettings() const { return overlaySettings_; }

    // Get singleton instance
    static PerformanceMonitor& getInstance();

    // Reset statistics
    void reset();

private:
    TimeSource now_;
    Clock::time_point frameStartTime_;
    std::unordered_map<std::string, Clock::time_point> sectionStartTimes_;

    FrameStats currentStats_;
    std::deque<double> frameTimeHistory_;

    PerformanceOverlaySettings overlaySettings_;

    mutable std::mutex mutex_;
};

// RAII helper for section timing
class ScopedSection {
public:
    explicit ScopedSection(const std::string& name);
    ~ScopedSection();

    // Disable copy
    ScopedSection(const ScopedSection&) = delete;
    ScopedSection& operator=(const ScopedSection&) = delete;

private:
    std::string name_;
};

// Convenience macros for profiling
#ifdef QUADMAPPER_ENABLE_PROFILING
    #define PERF_SCOPE(name) quadmapper::ScopedSection _perf_scope_##__LINE__(name)
    #define PERF_BEGIN(name) quadmapper::PerformanceMonitor::getInstance().beginSection(name)
    #define PERF_END(name) quadmapper::PerformanceMonitor::getInstance().endSection(name)
#else
    #define PERF_SCOPE(name) (void)0
    #define PERF_BEGIN(name) (void)0
    #define PERF_END(name) (void)0
#endif

} // namespace quadmapper
