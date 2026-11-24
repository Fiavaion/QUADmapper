#include "Texture.hpp"
#include "core/FileSystem.hpp"
#include "core/GlObjectStats.hpp"
#include "core/Logger.hpp"
#include <glad/glad.h>

#include <stb_image.h>

#include <limits>

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

// CPU pixel rows are tightly packed (e.g. RGB24 at any width). GL's default unpack
// alignment of 4 would read every row of a width not divisible by 4 at the wrong
// offset (sheared image, reads past the buffer), so uploads use alignment 1 and
// restore the caller's setting.
class ScopedTightUnpack {
public:
    ScopedTightUnpack() {
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    ~ScopedTightUnpack() { glPixelStorei(GL_UNPACK_ALIGNMENT, previous_); }
    ScopedTightUnpack(const ScopedTightUnpack&) = delete;
    ScopedTightUnpack& operator=(const ScopedTightUnpack&) = delete;

private:
    GLint previous_ = 4;
};

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
    // Top row first: quads sample v = 0 at their top edge, so the image shows upright.
    stbi_set_flip_vertically_on_load_thread(false);

    // Read through the wide path: stbi_load() opens its filename in the ANSI code page,
    // so an install folder with non-ASCII characters would fail.
    auto bytes = FileSystem::readBinaryFile(path, static_cast<size_t>(std::numeric_limits<int>::max()));
    if (!bytes || bytes.value().empty()) {
        Logger::error("Failed to load texture: {} - cannot read file", utf8FromPath(path));
        return false;
    }
    int width, height, channels;
    unsigned char* data =
        stbi_load_from_memory(bytes.value().data(), static_cast<int>(bytes.value().size()),
                              &width, &height, &channels, 0);

    if (!data) {
        const char* reason = stbi_failure_reason();
        Logger::error("Failed to load texture: {} - {}", utf8FromPath(path),
                      reason ? reason : "unknown");
        return false;
    }

    bool result = create(width, height, channels, data);
    stbi_image_free(data);

    if (result) {
        Logger::debug("Texture loaded: {} ({}x{}, {} channels)",
                     utf8FromPath(path.filename()), width, height, channels);
    }

    return result;
}

bool Texture::create(int width, int height, int channels, const unsigned char* data) {
    destroy();

    // Past GL_MAX_TEXTURE_SIZE glTexImage2D fails silently and the quad would sample black.
    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (maxSize > 0 && (width > maxSize || height > maxSize)) {
        // Callers retry every frame; name each oversize image once, not per frame.
        static thread_local int lastWidth = 0, lastHeight = 0;
        if (width != lastWidth || height != lastHeight) {
            lastWidth = width;
            lastHeight = height;
            Logger::error("Texture {}x{} is larger than this GPU's limit of {}x{}; it cannot be shown",
                          width, height, maxSize, maxSize);
        }
        return false;
    }

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
    {
        ScopedTightUnpack tight;
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);
    }

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

size_t Texture::compressedByteSize(GpuCompressedFormat fmt, int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    size_t bytesPerBlock = 0;
    switch (fmt) {
        case GpuCompressedFormat::BC1_RGB:   bytesPerBlock = 8; break;
        case GpuCompressedFormat::BC3_RGBA:
        case GpuCompressedFormat::BC3_YCoCg:
        case GpuCompressedFormat::BC7_RGBA:  bytesPerBlock = 16; break;
        case GpuCompressedFormat::None:
        default:                             return 0;
    }
    const size_t blocks = ((static_cast<size_t>(width) + 3) / 4) * ((static_cast<size_t>(height) + 3) / 4);
    return blocks * bytesPerBlock;
}

bool Texture::createCompressed(int width, int height, GpuCompressedFormat fmt,
                               const unsigned char* data, size_t size) {
    GLenum internalFormat = glCompressedInternalFormat(fmt);
    if (internalFormat == 0 || width <= 0 || height <= 0) {
        Logger::error("Texture::createCompressed: invalid format/dimensions ({}x{})", width, height);
        return false;
    }
    if (size != 0 && size != compressedByteSize(fmt, width, height)) {
        Logger::error("Texture::createCompressed: {} bytes is not the size of a {}x{} level", size, width, height);
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
    // With a pixel unpack buffer bound, data is an offset into it and may be null.
    if (!isValid() || !isCompressed() || size == 0) {
        return false;
    }

    GLenum internalFormat = glCompressedInternalFormat(compressedFormat_);
    if (internalFormat == 0) {
        return false;
    }
    if (size != compressedByteSize(compressedFormat_, width_, height_)) {
        Logger::error("Texture::updateCompressed: {} bytes is not the size of a {}x{} level", size, width_, height_);
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
    {
        ScopedTightUnpack tight;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, format, GL_UNSIGNED_BYTE, data);
    }
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
    {
        ScopedTightUnpack tight;
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, format, GL_UNSIGNED_BYTE, data);
    }
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
