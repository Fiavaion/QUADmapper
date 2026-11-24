#pragma once

#include "source/FrameData.hpp"  // GpuCompressedFormat

#include <cstdint>
#include <string>
#include <filesystem>

namespace quadmapper {

/**
 * @brief Sampling colorspace for the texture's stored bytes (ADR-044).
 *
 * HapQ stores scaled-YCoCg in a BC3 texture; A4's fragment shader reads this
 * flag to apply the YCoCg->RGB reconstruction. RGB is the default for every
 * ordinary (and BC1/BC3-RGBA/BC7) texture.
 */
enum class TextureColorSpace { RGB, YCoCg };

class Texture {
public:
    Texture();
    ~Texture();

    // Non-copyable
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // Move constructor/assignment
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    // Load from file
    bool loadFromFile(const std::filesystem::path& path);

    // Create from raw data
    bool create(int width, int height, int channels, const unsigned char* data);

    // Create an empty texture
    bool createEmpty(int width, int height, int channels = 4);

    // Create a GPU-compressed (BCn) texture from raw block bytes (ADR-044, HAP).
    // `data`/`size` may be the full BCn blob (uploaded immediately) or nullptr/0
    // to allocate storage only. A non-zero `size` that is not compressedByteSize()
    // fails (false) and leaves the texture as it was. Sets colorSpace() to YCoCg for BC3_YCoCg.
    // Caller must capability-gate (GpuCapabilities) before calling — the format
    // is a GL extension above the 3.3 floor.
    bool createCompressed(int width, int height, GpuCompressedFormat fmt,
                          const unsigned char* data = nullptr, size_t size = 0);

    // Re-upload the full compressed level for an existing compressed texture. While a
    // GL_PIXEL_UNPACK_BUFFER is bound, `data` is an offset into that buffer (as in GL).
    // False, with nothing uploaded, when `size` is not compressedByteSize() of the texture.
    bool updateCompressed(const unsigned char* data, size_t size);

    // Bytes one compressed level of width x height needs in `fmt`: 4x4 blocks of 8 bytes
    // (BC1) or 16 (BC3, BC7). 0 for None or a non-positive size.
    static size_t compressedByteSize(GpuCompressedFormat fmt, int width, int height);

    // Update texture data
    void update(const unsigned char* data);
    void update(int x, int y, int width, int height, const unsigned char* data);

    // Cleanup
    void destroy();

    // Bind/unbind
    void bind(uint32_t slot = 0) const;
    static void unbind();

    // Query
    bool isValid() const { return textureId_ != 0; }
    uint32_t getId() const { return textureId_; }
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }
    int getChannels() const { return channels_; }

    // GPU-compressed (BCn) state (ADR-044). compressedFormat() is None for
    // ordinary RGB(A) textures; colorSpace() drives A4's YCoCg->RGB shader.
    bool isCompressed() const { return compressedFormat_ != GpuCompressedFormat::None; }
    GpuCompressedFormat compressedFormat() const { return compressedFormat_; }
    TextureColorSpace colorSpace() const { return colorSpace_; }

    // Set texture parameters
    void setWrapMode(bool repeat);
    void setFilterMode(bool linear);

    // Set external GL texture ID (non-owning - won't be deleted)
    void setGLTextureId(uint32_t id, int width, int height, int channels = 3);

private:
    uint32_t textureId_ = 0;
    int width_ = 0;
    int height_ = 0;
    int channels_ = 0;
    bool ownsTexture_ = true;  // Whether we should delete the texture in destructor
    GpuCompressedFormat compressedFormat_ = GpuCompressedFormat::None;
    TextureColorSpace colorSpace_ = TextureColorSpace::RGB;
};

} // namespace quadmapper
