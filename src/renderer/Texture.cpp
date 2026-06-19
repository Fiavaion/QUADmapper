#include "Texture.hpp"
#include "core/GlObjectStats.hpp"
#include "core/Logger.hpp"
#include <glad/glad.h>

#include <stb_image.h>

// BCn internal-format tokens (ADR-044). Not present in our pinned GLAD 3.3
// loader header (they are extension tokens), so define the stable constants
// here. Values are fixed by GL_EXT_texture_compression_s3tc / _bptc.
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT  0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM    0x8E8C
#endif

namespace quadmapper {

namespace {

// Map our format enum to the GL internal format used by glCompressedTexImage2D.
// BC3_RGBA and BC3_YCoCg both upload as DXT5 — only the sampling colorspace
// differs (handled by colorSpace_ / A4's shader).
GLenum glCompressedInternalFormat(GpuCompressedFormat fmt) {
    switch (fmt) {
        case GpuCompressedFormat::BC1_RGB:   return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
        case GpuCompressedFormat::BC3_RGBA:  return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        case GpuCompressedFormat::BC3_YCoCg: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        case GpuCompressedFormat::BC7_RGBA:  return GL_COMPRESSED_RGBA_BPTC_UNORM;
        case GpuCompressedFormat::None:
        default:                             return 0;
    }
}

} // namespace

Texture::Texture() = default;

Texture::~Texture() {
    destroy();
}

Texture::Texture(Texture&& other) noexcept
    : textureId_(other.textureId_)
    , width_(other.width_)
    , height_(other.height_)
    , channels_(other.channels_)
    , ownsTexture_(other.ownsTexture_)
    , compressedFormat_(other.compressedFormat_)
    , colorSpace_(other.colorSpace_) {
    other.textureId_ = 0;
    other.width_ = 0;
    other.height_ = 0;
    other.channels_ = 0;
    other.ownsTexture_ = true;
    other.compressedFormat_ = GpuCompressedFormat::None;
    other.colorSpace_ = TextureColorSpace::RGB;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        destroy();
        textureId_ = other.textureId_;
        width_ = other.width_;
        height_ = other.height_;
        channels_ = other.channels_;
        ownsTexture_ = other.ownsTexture_;
        compressedFormat_ = other.compressedFormat_;
        colorSpace_ = other.colorSpace_;
        other.textureId_ = 0;
        other.width_ = 0;
        other.height_ = 0;
        other.channels_ = 0;
        other.ownsTexture_ = true;
        other.compressedFormat_ = GpuCompressedFormat::None;
        other.colorSpace_ = TextureColorSpace::RGB;
    }
    return *this;
}

bool Texture::loadFromFile(const std::filesystem::path& path) {
    // Set stb_image to flip vertically (OpenGL expects bottom-left origin)
    stbi_set_flip_vertically_on_load(true);

    // Load image data
    int width, height, channels;
    unsigned char* data = stbi_load(path.string().c_str(), &width, &height, &channels, 0);

    if (!data) {
        Logger::error("Failed to load texture: {} - {}", path.string(), stbi_failure_reason());
        return false;
    }

    bool result = create(width, height, channels, data);
    stbi_image_free(data);

    if (result) {
        Logger::debug("Texture loaded: {} ({}x{}, {} channels)",
                     path.filename().string(), width, height, channels);
    }

    return result;
}

bool Texture::create(int width, int height, int channels, const unsigned char* data) {
    destroy();

    width_ = width;
    height_ = height;
    channels_ = channels;
    ownsTexture_ = true;  // We own textures we create

    // Determine format
    GLenum internalFormat = GL_RGBA8;
    GLenum format = GL_RGBA;

    switch (channels) {
        case 1:
            internalFormat = GL_R8;
            format = GL_RED;
            break;
        case 2:
            internalFormat = GL_RG8;
            format = GL_RG;
            break;
        case 3:
            internalFormat = GL_RGB8;
            format = GL_RGB;
            break;
        case 4:
        default:
            internalFormat = GL_RGBA8;
            format = GL_RGBA;
            break;
    }

    // Create texture
    glGenTextures(1, &textureId_);
    GlObjectStats::onTextureCreated();
    glBindTexture(GL_TEXTURE_2D, textureId_);

    // Upload data
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);

    // Generate mipmaps
    glGenerateMipmap(GL_TEXTURE_2D);

    // Set default parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

bool Texture::createEmpty(int width, int height, int channels) {
    return create(width, height, channels, nullptr);
}

bool Texture::createCompressed(int width, int height, GpuCompressedFormat fmt,
                               const unsigned char* data, size_t size) {
    GLenum internalFormat = glCompressedInternalFormat(fmt);
    if (internalFormat == 0 || width <= 0 || height <= 0) {
        Logger::error("Texture::createCompressed: invalid format/dimensions ({}x{})", width, height);
        return false;
    }

    destroy();

    width_ = width;
    height_ = height;
    // No per-pixel channel concept for BCn; use 4 as a sentinel so isValid()
    // and any RGBA-assuming caller stay sane. The compressed path never reads
    // channels_ for an upload format.
    channels_ = 4;
    ownsTexture_ = true;  // We own textures we create
    compressedFormat_ = fmt;
    colorSpace_ = (fmt == GpuCompressedFormat::BC3_YCoCg)
                      ? TextureColorSpace::YCoCg
                      : TextureColorSpace::RGB;

    glGenTextures(1, &textureId_);
    GlObjectStats::onTextureCreated();
    glBindTexture(GL_TEXTURE_2D, textureId_);

    // Allocate (and upload, if data given) the single compressed level. BCn
    // textures cannot be glGenerateMipmap'd, so we use a non-mipmap min filter.
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0,
                           static_cast<GLsizei>(size),
                           (data && size) ? data : nullptr);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

bool Texture::updateCompressed(const unsigned char* data, size_t size) {
    if (!isValid() || !isCompressed() || !data || size == 0) {
        return false;
    }

    GLenum internalFormat = glCompressedInternalFormat(compressedFormat_);
    if (internalFormat == 0) {
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, textureId_);
    // Compressed levels are re-specified whole; a full-image SubImage matches
    // the level allocated by createCompressed().
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_,
                              internalFormat, static_cast<GLsizei>(size), data);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void Texture::update(const unsigned char* data) {
    if (!isValid()) return;

    GLenum format = GL_RGBA;
    switch (channels_) {
        case 1: format = GL_RED; break;
        case 2: format = GL_RG; break;
        case 3: format = GL_RGB; break;
        case 4: format = GL_RGBA; break;
    }

    glBindTexture(GL_TEXTURE_2D, textureId_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, format, GL_UNSIGNED_BYTE, data);
    // create() built the texture with a mipmap min-filter; a full-texture update
    // must refresh the mip chain, else minified sampling reads the stale (empty)
    // levels and the quad renders black.
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::update(int x, int y, int width, int height, const unsigned char* data) {
    if (!isValid()) return;

    GLenum format = GL_RGBA;
    switch (channels_) {
        case 1: format = GL_RED; break;
        case 2: format = GL_RG; break;
        case 3: format = GL_RGB; break;
        case 4: format = GL_RGBA; break;
    }

    glBindTexture(GL_TEXTURE_2D, textureId_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, format, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::destroy() {
    if (textureId_ != 0) {
        if (ownsTexture_) {
            glDeleteTextures(1, &textureId_);
            GlObjectStats::onTextureDestroyed();
        }
        textureId_ = 0;
        width_ = 0;
        height_ = 0;
        channels_ = 0;
        ownsTexture_ = true;
        compressedFormat_ = GpuCompressedFormat::None;
        colorSpace_ = TextureColorSpace::RGB;
    }
}

void Texture::setGLTextureId(uint32_t id, int width, int height, int channels) {
    destroy();  // Clean up any existing texture
    textureId_ = id;
    width_ = width;
    height_ = height;
    channels_ = channels;
    ownsTexture_ = false;  // We don't own this texture, don't delete it
    compressedFormat_ = GpuCompressedFormat::None;
    colorSpace_ = TextureColorSpace::RGB;
}

void Texture::bind(uint32_t slot) const {
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, textureId_);
}

void Texture::unbind() {
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::setWrapMode(bool repeat) {
    if (!isValid()) return;

    GLenum mode = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glBindTexture(GL_TEXTURE_2D, textureId_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mode);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::setFilterMode(bool linear) {
    if (!isValid()) return;

    glBindTexture(GL_TEXTURE_2D, textureId_);
    if (linear) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace quadmapper
