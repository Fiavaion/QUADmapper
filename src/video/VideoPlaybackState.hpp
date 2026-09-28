#pragma once

#include "core/Types.hpp"

#include <cstdint>

namespace quadmapper {

/**
 * @brief Snapshot of a video player's playback, refreshed on every update
 *
 * The playback clock (PlaybackTransport) is the source of truth; this is what the
 * UI and the source wrapper read.
 */
struct VideoPlaybackState {
    PlaybackState state = PlaybackState::STOPPED;
    double position = 0.0;          // Current position in seconds
    double duration = 0.0;          // Total duration in seconds
    float speed = 1.0f;             // Playback speed (0.1 - 4.0)
    LoopMode loopMode = LoopMode::LOOP;  // as requested (see VideoPlayer::getLoopModeNotice)
    int direction = 1;              // -1 on a Ping-Pong reverse leg
    int64_t currentFrame = 0;
    int64_t totalFrames = 0;

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
        direction = 1;
        currentFrame = 0;
    }
};

} // namespace quadmapper
