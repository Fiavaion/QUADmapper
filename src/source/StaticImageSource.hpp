#pragma once

#include "TextureSource.hpp"
#include "FrameData.hpp"
#include <filesystem>

namespace quadmapper {

/**
 * @brief Texture source for static images
 *
 * Loads an image from disk once and provides it as a static texture.
 * After initial load, the same frame data is returned on every update.
 *
 * This is a CPU-based source: pixel data is loaded to RAM and then
 * uploaded to GPU by SourceManager.
 */
class StaticImageSource : public TextureSource {
public:
    /**
     * @brief Construct a static image source
     * @param path Path to image file
     */
    explicit StaticImageSource(const std::filesystem::path& path);
    ~StaticImageSource() override;

    // --- Identity ---
    const UUID& getId() const override { return id_; }
    std::string getName() const override { return name_; }
    SourceType getType() const override { return SourceType::STATIC_IMAGE; }
    std::string getTypeName() const override { return "Image"; }

    // --- Lifecycle ---
    bool initialize() override;
    void shutdown() override;
    bool isReady() const override { return ready_; }

    // --- Per-Frame Update ---
    bool update(double deltaTime) override;

    // --- Frame Access ---
    bool hasNewFrame() const override { return hasNewFrame_; }
    const FrameData* getCurrentFrame() const override;
    void markFrameConsumed() override;

    // --- Capabilities ---
    SourceCapabilities getCapabilities() const override;

    // --- Dimensions ---
    int getWidth() const override { return frame_.width; }
    int getHeight() const override { return frame_.height; }

    // --- Image-specific ---
    const std::filesystem::path& getPath() const { return path_; }

private:
    UUID id_ = UUID::generate();  // fresh id; null would make SourceManager reject it
    std::string name_;
    std::filesystem::path path_;
    FrameData frame_;
    bool ready_ = false;
    bool hasNewFrame_ = false;
    bool firstFrame_ = true;
};

} // namespace quadmapper
