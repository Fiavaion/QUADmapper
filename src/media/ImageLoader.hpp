#pragma once

#include "core/Result.hpp"
#include <filesystem>
#include <vector>
#include <cstdint>

namespace quadmapper {

/**
 * @brief Container for loaded image pixel data
 */
struct ImageData {
    int width = 0;
    int height = 0;
    int channels = 0;               // 1=R, 2=RG, 3=RGB, 4=RGBA
    std::vector<uint8_t> pixels;    // Raw pixel data

    bool isValid() const {
        return width > 0 && height > 0 && channels > 0 && !pixels.empty();
    }

    size_t getDataSize() const {
        return static_cast<size_t>(width) * height * channels;
    }
};

/**
 * @brief Static utility class for loading images from disk
 *
 * Uses stb_image internally to support common image formats:
 * PNG, JPEG, BMP, GIF, TGA, PSD, HDR, PIC, PPM, PGM.
 * TIF/TIFF decode through the system instead (WIC on Windows, ImageIO on macOS): first frame
 * only, always converted to RGBA (4 channels unless others are requested).
 *
 * Pixels are returned top row first (both decode paths), the same order as
 * video, camera and procedural frames: texture v = 0 is the image's top.
 */
class ImageLoader {
public:
    /**
     * @brief Load an image from file
     * @param path Path to image file
     * @return ImageData on success, Error on failure
     */
    static Result<ImageData, Error> load(const std::filesystem::path& path);

    /**
     * @brief Load an image with specific channel count
     * @param path Path to image file
     * @param desiredChannels Desired channels (1-4), or 0 for auto-detect
     * @return ImageData on success, Error on failure
     *
     * Useful for forcing RGBA (4 channels) even if source is RGB.
     */
    static Result<ImageData, Error> loadWithChannels(
        const std::filesystem::path& path,
        int desiredChannels
    );

    /**
     * @brief Check if a file extension is a supported image format
     * @param path File path to check
     * @return true if the extension is supported
     */
    static bool isSupported(const std::filesystem::path& path);
};

} // namespace quadmapper
