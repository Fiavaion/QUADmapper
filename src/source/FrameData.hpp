#pragma once

#include <vector>
#include <cstdint>

namespace quadmapper {

/**
 * @brief GPU-compressed (BCn) frame format — for HAP playback (ADR-044).
 *
 * When a FrameData's compressedFormat is not None, `pixels` holds raw BCn
 * block bytes ready for glCompressedTexSubImage2D (NOT decoded RGB(A) pixels),
 * `channels` is unused, and getDataSize() returns the byte count directly.
 * BC3_YCoCg uploads as an ordinary BC3 texture but the fragment shader applies
 * a YCoCg->RGB conversion (HapQ); the colorspace is carried on the GPU Texture.
 */
enum class GpuCompressedFormat {
    None,       // ordinary RGB/RGBA pixels in `pixels`
    BC1_RGB,    // HAP       -> GL_COMPRESSED_RGB_S3TC_DXT1_EXT
    BC3_RGBA,   // HAP Alpha -> GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
    BC3_YCoCg,  // HAP Q     -> GL_COMPRESSED_RGBA_S3TC_DXT5_EXT + YCoCg->RGB shader
    BC7_RGBA,   // HAP R     -> GL_COMPRESSED_RGBA_BPTC_UNORM
};

/**
 * @brief CPU-side frame data ready for GPU upload
 *
 * This is a format-agnostic staging buffer that holds pixel data
 * before it's uploaded to an OpenGL texture. All texture sources
 * produce FrameData that the SourceManager uploads to GPU.
 */
struct FrameData {
    std::vector<uint8_t> pixels;    // RGB/RGBA pixels, OR raw BCn bytes when compressed
    int width = 0;
    int height = 0;
    int channels = 3;               // 3 = RGB, 4 = RGBA (unused when compressed)
    uint64_t timestamp = 0;         // Presentation timestamp (for sync)
    uint64_t frameNumber = 0;       // Sequential frame number

    // GPU-compressed (HAP/BCn) path (ADR-044). None = ordinary pixel upload.
    GpuCompressedFormat compressedFormat = GpuCompressedFormat::None;
    bool isCompressed() const { return compressedFormat != GpuCompressedFormat::None; }

    /**
     * @brief Check if frame contains valid data
     */
    bool isValid() const {
        return !pixels.empty() && width > 0 && height > 0 &&
               (isCompressed() || channels > 0);
    }

    /**
     * @brief Get expected data size in bytes (BCn byte count when compressed)
     */
    size_t getDataSize() const {
        if (isCompressed()) return pixels.size();
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
        compressedFormat = GpuCompressedFormat::None;
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
