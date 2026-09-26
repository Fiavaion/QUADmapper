#include "PerformanceMonitor.hpp"
#include "Logger.hpp"
#include <algorithm>
#include <numeric>
#include <utility>

namespace quadmapper {

// ScopedTimer implementation
ScopedTimer::ScopedTimer(const std::string& name)
    : name_(name)
    , startTime_(Clock::now())
{
}

ScopedTimer::~ScopedTimer() {
    double elapsed = getElapsedMs();
    LOG_TRACE("Timer [{}]: {:.3f} ms", name_, elapsed);
}

double ScopedTimer::getElapsedMs() const {
    auto now = Clock::now();
    return std::chrono::duration<double, std::milli>(now - startTime_).count();
}

// PerformanceMonitor implementation
PerformanceMonitor::PerformanceMonitor(TimeSource now)
    : now_(now ? std::move(now) : TimeSource([] { return Clock::now(); }))
{
}

PerformanceMonitor& PerformanceMonitor::getInstance() {
    static PerformanceMonitor instance;
    return instance;
}

void PerformanceMonitor::beginFrame() {
    std::lock_guard<std::mutex> lock(mutex_);

    frameStartTime_ = now_();

    // Reset per-frame stats
    currentStats_.drawCalls = 0;
    currentStats_.triangles = 0;
}

void PerformanceMonitor::endFrame() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = now_();
    double frameTime = std::chrono::duration<double, std::milli>(now - frameStartTime_).count();

    currentStats_.frameTimeMs = frameTime;

    // Add to history
    frameTimeHistory_.push_back(frameTime);
    if (frameTimeHistory_.size() > HISTORY_SIZE) {
        frameTimeHistory_.pop_front();
    }
}

void PerformanceMonitor::beginSection(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    sectionStartTimes_[name] = now_();
}

void PerformanceMonitor::endSection(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sectionStartTimes_.find(name);
    if (it == sectionStartTimes_.end()) {
        LOG_WARNING("endSection called without matching beginSection: {}", name);
        return;
    }

    auto now = now_();
    double elapsed = std::chrono::duration<double, std::milli>(now - it->second).count();
    sectionStartTimes_.erase(it);

    // Update the appropriate stat based on section name
    if (name == "render") {
        currentStats_.renderTimeMs = elapsed;
    } else if (name == "update") {
        currentStats_.updateTimeMs = elapsed;
    } else if (name == "ui") {
        currentStats_.uiTimeMs = elapsed;
    }
}

void PerformanceMonitor::recordDrawCalls(int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentStats_.drawCalls += count;
}

void PerformanceMonitor::recordTriangles(int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentStats_.triangles += count;
}

void PerformanceMonitor::recordTextureMemory(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentStats_.textureMemoryMB = bytes / (1024 * 1024);
}

double PerformanceMonitor::getAverageFPS() const {
    double avgFrameTime = getAverageFrameTime();
    if (avgFrameTime <= 0.0) return 0.0;
    return 1000.0 / avgFrameTime;
}

double PerformanceMonitor::getAverageFrameTime() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (frameTimeHistory_.empty()) return 0.0;

    double sum = std::accumulate(frameTimeHistory_.begin(), frameTimeHistory_.end(), 0.0);
    return sum / static_cast<double>(frameTimeHistory_.size());
}

double PerformanceMonitor::getMinFrameTime() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (frameTimeHistory_.empty()) return 0.0;

    return *std::min_element(frameTimeHistory_.begin(), frameTimeHistory_.end());
}

double PerformanceMonitor::getMaxFrameTime() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (frameTimeHistory_.empty()) return 0.0;

    return *std::max_element(frameTimeHistory_.begin(), frameTimeHistory_.end());
}

void PerformanceMonitor::reset() {
    std::lock_guard<std::mutex> lock(mutex_);

    currentStats_ = FrameStats{};
    frameTimeHistory_.clear();
    sectionStartTimes_.clear();
}

// ScopedSection implementation
ScopedSection::ScopedSection(const std::string& name)
    : name_(name)
{
    PerformanceMonitor::getInstance().beginSection(name_);
}

ScopedSection::~ScopedSection() {
    PerformanceMonitor::getInstance().endSection(name_);
}

} // namespace quadmapper
