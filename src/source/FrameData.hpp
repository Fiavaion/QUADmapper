#pragma once

#include <vector>
#include <cstdint>

namespace quadmapper {

/**
 * @brief CPU-side frame data ready for GPU upload
 *
 * This is a format-agnostic staging buffer that holds pixel data
 * before it's uploaded to an OpenGL texture. All texture sources
 * produce FrameData that the SourceManager uploads to GPU.
 */
struct FrameData {
    std::vector<uint8_t> pixels;    // Raw pixel data (RGB or RGBA)
    int width = 0;
    int height = 0;
    int channels = 3;               // 3 = RGB, 4 = RGBA
    uint64_t timestamp = 0;         // Presentation timestamp (for sync)
    uint64_t frameNumber = 0;       // Sequential frame number

    /**
     * @brief Check if frame contains valid data
     */
    bool isValid() const {
        return !pixels.empty() && width > 0 && height > 0 && channels > 0;
    }

    /**
     * @brief Get expected pixel data size in bytes
     */
    size_t getDataSize() const {
        return static_cast<size_t>(width) * height * channels;
    }

    /**
     * @brief Clear all data
     */
    void clear() {
        pixels.clear();
        width = 0;
        height = 0;
        channels = 3;
        timestamp = 0;
        frameNumber = 0;
    }

    /**
     * @brief Allocate pixel buffer for given dimensions
     */
    void allocate(int w, int h, int ch = 3) {
        width = w;
        height = h;
        channels = ch;
        pixels.resize(getDataSize());
    }
};

} // namespace quadmapper
