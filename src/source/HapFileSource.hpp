#pragma once

#include "TextureSource.hpp"
#include "FrameData.hpp"
#include <filesystem>
#include <memory>
#include <string>

namespace quadmapper {

/**
 * @brief Texture source for HAP-codec video files (ADR-044, QM-080)
 *
 * HAP frames are already GPU-compressed BCn textures. This source demuxes the
 * container with FFmpeg (avformat) but does NOT run FFmpeg's HAP *decoder* —
 * each demuxed packet IS the raw HAP frame, which is parsed (hap::parseHapFrame)
 * straight to BCn bytes and staged as a compressed FrameData for the main thread
 * to upload via glCompressedTexSubImage2D (A3).
 *
 * Reports SourceType::VIDEO_FILE so the UI / serialization treat HAP as a video
 * uniformly. FFmpeg and the demux thread live entirely behind the .cpp pimpl
 * (ADR-011); no libav* headers leak here.
 *
 * Threading: demux + parse run on a background thread; the latest decoded BCn
 * frame is staged under a mutex and handed to the main thread (no GL here).
 */
class HapFileSource : public PlayableSource {
public:
    explicit HapFileSource(const std::filesystem::path& path);
    ~HapFileSource() override;

    // --- TextureSource: Identity ---
    const UUID& getId() const override { return id_; }
    std::string getName() const override { return name_; }
    SourceType getType() const override { return SourceType::VIDEO_FILE; }
    std::string getTypeName() const override { return "Video (HAP)"; }

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

    // --- PlayableSource: Health ---
    std::string getErrorMessage() const override;

    // --- HAP-specific ---
    const std::filesystem::path& getPath() const { return path_; }

private:
    class Impl;  // FFmpeg + demux thread behind the pimpl (ADR-011)

    UUID id_ = UUID::generate();  // fresh id; null would make SourceManager reject it
    std::string name_;
    std::filesystem::path path_;
    std::unique_ptr<Impl> impl_;
    FrameData frame_;       // staged compressed frame for main-thread upload
    bool ready_ = false;
    bool hasNewFrame_ = false;
};

} // namespace quadmapper
