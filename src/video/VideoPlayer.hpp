#pragma once

#include "VideoDecoder.hpp"
#include "VideoPlaybackState.hpp"
#include <filesystem>
#include <memory>
#include <functional>

namespace quadmapper {

/**
 * @brief High-level video player with playback controls
 *
 * Combines VideoDecoder with playback state management to provide
 * a complete video playback solution.
 */
class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    // Non-copyable, movable
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;
    VideoPlayer(VideoPlayer&&) noexcept;
    VideoPlayer& operator=(VideoPlayer&&) noexcept;

    /**
     * @brief Load a video file
     * @param path Path to video file
     * @return true if successful
     */
    bool load(const std::filesystem::path& path);

    /**
     * @brief Unload the current video
     */
    void unload();

    /**
     * @brief Check if a video is loaded
     */
    bool isLoaded() const;

    // --- Playback Controls ---

    /**
     * @brief Start or resume playback
     */
    void play();

    /**
     * @brief Pause playback
     */
    void pause();

    /**
     * @brief Stop playback and reset to beginning
     */
    void stop();

    /**
     * @brief Toggle between play and pause
     */
    void togglePlayPause();

    /**
     * @brief Seek to a specific time
     * @param timeSeconds Target time in seconds
     */
    void seek(double timeSeconds);

    /**
     * @brief Seek to a specific progress position
     * @param progress 0.0 = start, 1.0 = end
     */
    void seekProgress(float progress);

    /**
     * @brief Skip forward/backward by a number of seconds
     */
    void skip(double seconds);

    // --- Speed Control ---

    /**
     * @brief Set playback speed
     * @param speed Speed multiplier (0.1 - 4.0)
     */
    void setSpeed(float speed);

    /**
     * @brief Get current playback speed
     */
    float getSpeed() const;

    // --- Loop Control ---

    /**
     * @brief Set loop mode
     */
    void setLoopMode(LoopMode mode);

    /**
     * @brief Get current loop mode
     */
    LoopMode getLoopMode() const;

    // --- State ---

    /**
     * @brief Get current playback state
     */
    const VideoPlaybackState& getState() const;

    /**
     * @brief Get video info
     */
    const VideoInfo& getVideoInfo() const;

    /**
     * @brief Get file path
     */
    const std::filesystem::path& getPath() const;

    // --- Frame Access ---

    /**
     * @brief Update the player (call each frame)
     * @param deltaTime Time since last update in seconds
     * @return true if a new frame is ready
     */
    bool update(double deltaTime);

    /**
     * @brief Get the current frame for display
     * @return Current frame, or invalid frame if none ready
     */
    const VideoFrame& getCurrentFrame() const;

    /**
     * @brief Check if a new frame is ready
     */
    bool hasNewFrame() const;

    /**
     * @brief Mark the current frame as consumed
     */
    void consumeFrame();

    // --- Callbacks ---

    using EndCallback = std::function<void()>;

    /**
     * @brief Set callback for when video ends (in ONCE mode)
     */
    void setEndCallback(EndCallback callback);

private:
    void handleEndOfVideo();
    void updatePlaybackPosition(double deltaTime);

    std::unique_ptr<VideoDecoder> decoder_;
    VideoPlaybackState state_;
    VideoFrame currentFrame_;
    std::filesystem::path path_;

    bool hasNewFrame_ = false;
    bool needsSeek_ = false;
    double seekTarget_ = 0.0;

    EndCallback endCallback_;
};

} // namespace quadmapper
