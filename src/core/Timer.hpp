#pragma once

#include <chrono>

namespace quadmapper {

class Timer {
public:
    Timer();

    void start();
    void stop();
    void reset();

    bool isRunning() const { return running_; }
    double elapsedSeconds() const;
    double elapsedMillis() const;

private:
    std::chrono::high_resolution_clock::time_point startTime_;
    std::chrono::high_resolution_clock::duration accumulated_;
    bool running_;
};

// Frame timer for tracking delta time
class FrameTimer {
public:
    FrameTimer();

    void update();
    float getDeltaTime() const { return static_cast<float>(deltaTime_); }
    float getFPS() const { return static_cast<float>(fps_); }

private:
    std::chrono::high_resolution_clock::time_point lastTime_;
    double deltaTime_;
    double fps_;
    double fpsAccumulator_;
    int frameCount_;
};

} // namespace quadmapper
