#pragma once

#include "TextureSource.hpp"
#include "FrameData.hpp"
#include "video/VideoPlayer.hpp"
#include <filesystem>
#include <memory>

namespace quadmapper {

/**
 * @brief Texture source for video files
 *
 * Wraps VideoPlayer to provide video playback as a texture source.
 * Supports play/pause/seek controls via the PlayableSource interface.
 *
 * Video decoding happens on a background thread; frames are transferred
 * to the main thread for GPU upload.
 */
class VideoFileSource : public PlayableSource {
public:
    /**
     * @brief Construct a video file source
     * @param path Path to video file
     */
    explicit VideoFileSource(const std::filesystem::path& path);
    ~VideoFileSource() override;

    // --- TextureSource: Identity ---
    const UUID& getId() const override { return id_; }
    std::string getName() const override { return name_; }
    SourceType getType() const override { return SourceType::VIDEO_FILE; }
    std::string getTypeName() const override { return "Video"; }

    // --- TextureSource: Lifecycle ---
    bool initialize() override;
    void shutdown() override;
    bool isReady() const override { return ready_; }

    // --- TextureSource: Per-Frame Update ---
    bool update(double deltaTime) override;

    // --- TextureSource: Frame Access ---
    bool hasNewFrame() const override { return hasNewFrame_; }
    const FrameData* getCurrentFrame() const override;
    void markFrameConsumed() override;

    // --- TextureSource: Capabilities ---
    SourceCapabilities getCapabilities() const override;

    // --- TextureSource: Dimensions ---
    int getWidth() const override;
    int getHeight() const override;

    // --- PlayableSource: Playback Controls ---
    void play() override;
    void pause() override;
    void stop() override;
    void togglePlayPause() override;

    // --- PlayableSource: Seeking ---
    bool canSeek() const override { return true; }
    void seek(double timeSeconds) override;
    void seekProgress(float progress) override;

    // --- PlayableSource: State ---
    PlaybackState getPlaybackState() const override;
    double getCurrentTime() const override;
    double getDuration() const override;
    float getProgress() const override;

    // --- PlayableSource: Speed ---
    void setSpeed(float speed) override;
    float getSpeed() const override;

    // --- PlayableSource: Loop ---
    void setLoopMode(LoopMode mode) override;
    LoopMode getLoopMode() const override;

    // --- Video-specific ---
    const std::filesystem::path& getPath() const { return path_; }
    const VideoInfo& getVideoInfo() const;

private:
    void convertVideoFrame(const VideoFrame& videoFrame);

    UUID id_ = UUID::generate();  // fresh id; null would make SourceManager reject it
    std::string name_;
    std::filesystem::path path_;
    std::unique_ptr<VideoPlayer> player_;
    FrameData frame_;
    bool ready_ = false;
    bool hasNewFrame_ = false;
};

} // namespace quadmapper
