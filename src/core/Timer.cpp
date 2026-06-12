#include "Timer.hpp"

namespace quadmapper {

Timer::Timer()
    : startTime_(std::chrono::high_resolution_clock::now())
    , accumulated_(std::chrono::high_resolution_clock::duration::zero())
    , running_(false)
{
}

void Timer::start() {
    if (!running_) {
        startTime_ = std::chrono::high_resolution_clock::now();
        running_ = true;
    }
}

void Timer::stop() {
    if (running_) {
        auto now = std::chrono::high_resolution_clock::now();
        accumulated_ += now - startTime_;
        running_ = false;
    }
}

void Timer::reset() {
    accumulated_ = std::chrono::high_resolution_clock::duration::zero();
    startTime_ = std::chrono::high_resolution_clock::now();
}

double Timer::elapsedSeconds() const {
    auto duration = accumulated_;
    if (running_) {
        duration += std::chrono::high_resolution_clock::now() - startTime_;
    }
    return std::chrono::duration<double>(duration).count();
}

double Timer::elapsedMillis() const {
    return elapsedSeconds() * 1000.0;
}

FrameTimer::FrameTimer()
    : lastTime_(std::chrono::high_resolution_clock::now())
    , deltaTime_(0.0)
    , fps_(0.0)
    , fpsAccumulator_(0.0)
    , frameCount_(0)
{
}

void FrameTimer::update() {
    auto now = std::chrono::high_resolution_clock::now();
    deltaTime_ = std::chrono::duration<double>(now - lastTime_).count();
    lastTime_ = now;

    // Calculate FPS
    fpsAccumulator_ += deltaTime_;
    frameCount_++;

    // Provide immediate FPS estimate from deltaTime
    if (deltaTime_ > 0.0001) {
        double instantFps = 1.0 / deltaTime_;
        // Smooth FPS: blend with previous value
        if (fps_ == 0.0) {
            fps_ = instantFps;
        } else {
            fps_ = fps_ * 0.9 + instantFps * 0.1;
        }
    }

    // Reset accumulator every second for accurate long-term FPS
    if (fpsAccumulator_ >= 1.0) {
        fps_ = frameCount_ / fpsAccumulator_;
        fpsAccumulator_ = 0.0;
        frameCount_ = 0;
    }
}

} // namespace quadmapper
