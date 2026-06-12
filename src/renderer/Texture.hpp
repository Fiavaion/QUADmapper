#pragma once

#include <cstdint>
#include <string>
#include <filesystem>

namespace quadmapper {

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
};

} // namespace quadmapper
