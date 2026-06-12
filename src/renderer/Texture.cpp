#include "Texture.hpp"
#include "core/Logger.hpp"
#include <glad/glad.h>

#include <stb_image.h>

namespace quadmapper {

Texture::Texture() = default;

Texture::~Texture() {
    destroy();
}

Texture::Texture(Texture&& other) noexcept
    : textureId_(other.textureId_)
    , width_(other.width_)
    , height_(other.height_)
    , channels_(other.channels_)
    , ownsTexture_(other.ownsTexture_) {
    other.textureId_ = 0;
    other.width_ = 0;
    other.height_ = 0;
    other.channels_ = 0;
    other.ownsTexture_ = true;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        destroy();
        textureId_ = other.textureId_;
        width_ = other.width_;
        height_ = other.height_;
        channels_ = other.channels_;
        ownsTexture_ = other.ownsTexture_;
        other.textureId_ = 0;
        other.width_ = 0;
        other.height_ = 0;
        other.channels_ = 0;
        other.ownsTexture_ = true;
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
        }
        textureId_ = 0;
        width_ = 0;
        height_ = 0;
        channels_ = 0;
        ownsTexture_ = true;
    }
}

void Texture::setGLTextureId(uint32_t id, int width, int height, int channels) {
    destroy();  // Clean up any existing texture
    textureId_ = id;
    width_ = width;
    height_ = height;
    channels_ = channels;
    ownsTexture_ = false;  // We don't own this texture, don't delete it
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
