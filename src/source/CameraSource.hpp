#pragma once

#include "TextureSource.hpp"
#include "FrameData.hpp"
#include "core/UUID.hpp"
#include <memory>
#include <string>
#include <filesystem>

namespace quadmapper {

class VideoDecoder;
struct VideoFrame;

/**
 * @brief Live webcam / capture-device texture source (Windows dshow).
 *
 * Wraps a VideoDecoder opened via openCamera(). Unlike VideoFileSource it is a
 * plain (non-playable) TextureSource: live capture has no timeline, seek, or
 * duration. Frames decode on a background thread and are drained to the newest
 * one each update() to keep the preview low-latency. See QM-055.
 */
class CameraSource : public TextureSource {
public:
    /**
     * @param deviceName dshow FriendlyName; empty = first enumerated device.
     * @param width/height/fps desired capture mode; 0 = device default.
     */
    explicit CameraSource(std::string deviceName, int width = 0, int height = 0, double fps = 0.0);
    ~CameraSource() override;

    // --- Identity ---
    const UUID& getId() const override { return id_; }
    std::string getName() const override { return name_; }
    SourceType getType() const override { return SourceType::WEBCAM; }
    std::string getTypeName() const override { return "Camera"; }

    // --- Lifecycle ---
    bool initialize() override;
    void shutdown() override;
    bool isReady() const override { return ready_; }

    // --- Per-frame update ---
    bool update(double deltaTime) override;

    // --- Frame access ---
    bool hasNewFrame() const override { return hasNewFrame_; }
    const FrameData* getCurrentFrame() const override;
    void markFrameConsumed() override;

    // --- Capabilities / dimensions ---
    SourceCapabilities getCapabilities() const override;
    int getWidth() const override { return width_; }
    int getHeight() const override { return height_; }

    // --- Camera-specific ---
    const std::string& getDeviceName() const { return deviceName_; }

    /**
     * @brief Save the current frame to a PNG file (the AI sidecar's input, AC2).
     * @return true on success; false if no valid frame yet.
     */
    bool captureCurrentFrameToPng(const std::filesystem::path& path) const;

private:
    void convertFrame(const VideoFrame& vf);

    UUID id_;
    std::string name_;
    std::string deviceName_;
    int reqWidth_ = 0;
    int reqHeight_ = 0;
    double reqFps_ = 0.0;
    std::unique_ptr<VideoDecoder> decoder_;
    FrameData frame_;
    int width_ = 0;
    int height_ = 0;
    bool ready_ = false;
    bool hasNewFrame_ = false;
};

} // namespace quadmapper
