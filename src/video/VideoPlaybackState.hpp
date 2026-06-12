#pragma once

#include "core/Types.hpp"
#include <chrono>

namespace quadmapper {

/**
 * @brief Complete playback state for a video player
 */
struct VideoPlaybackState {
    PlaybackState state = PlaybackState::STOPPED;
    double position = 0.0;          // Current position in seconds
    double duration = 0.0;          // Total duration in seconds
    float speed = 1.0f;             // Playback speed (0.1 - 4.0)
    LoopMode loopMode = LoopMode::LOOP;
    bool isLooping = false;         // Currently in a loop iteration
    int64_t currentFrame = 0;
    int64_t totalFrames = 0;

    // Timing
    std::chrono::steady_clock::time_point lastUpdateTime;
    double accumulatedTime = 0.0;   // Time since last frame

    // Helper methods
    bool isPlaying() const { return state == PlaybackState::PLAYING; }
    bool isPaused() const { return state == PlaybackState::PAUSED; }
    bool isStopped() const { return state == PlaybackState::STOPPED; }
    bool isFinished() const { return state == PlaybackState::FINISHED; }

    float getProgress() const {
        if (duration <= 0.0) return 0.0f;
        return static_cast<float>(position / duration);
    }

    void reset() {
        state = PlaybackState::STOPPED;
        position = 0.0;
        currentFrame = 0;
        accumulatedTime = 0.0;
        isLooping = false;
    }
};

} // namespace quadmapper
