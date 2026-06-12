#include "ImageLoader.hpp"
#include "core/Logger.hpp"
#include <stb_image.h>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace quadmapper {

Result<ImageData, Error> ImageLoader::load(const std::filesystem::path& path) {
    // Check if file exists
    if (!std::filesystem::exists(path)) {
        return Err<ImageData>("File not found: " + path.string());
    }

    // Set stb_image to flip vertically (OpenGL expects bottom-left origin)
    stbi_set_flip_vertically_on_load(true);

    // Load image data
    int width, height, channels;
    unsigned char* data = stbi_load(path.string().c_str(), &width, &height, &channels, 0);

    if (!data) {
        const char* reason = stbi_failure_reason();
        Logger::error("Failed to load image: {} - {}", path.string(), reason ? reason : "unknown");
        return Err<ImageData>("Failed to load image: " + std::string(reason ? reason : "unknown"));
    }

    // Create ImageData and copy pixels
    ImageData result;
    result.width = width;
    result.height = height;
    result.channels = channels;

    size_t dataSize = static_cast<size_t>(width) * height * channels;
    result.pixels.resize(dataSize);
    std::memcpy(result.pixels.data(), data, dataSize);

    // Free stb_image's data
    stbi_image_free(data);

    Logger::debug("Image loaded: {} ({}x{}, {} channels)",
                  path.filename().string(), width, height, channels);

    return Ok(std::move(result));
}

Result<ImageData, Error> ImageLoader::loadWithChannels(
    const std::filesystem::path& path,
    int desiredChannels)
{
    // Check if file exists
    if (!std::filesystem::exists(path)) {
        return Err<ImageData>("File not found: " + path.string());
    }

    // Validate desired channels
    if (desiredChannels < 0 || desiredChannels > 4) {
        return Err<ImageData>("Invalid channel count: " + std::to_string(desiredChannels));
    }

    // Set stb_image to flip vertically
    stbi_set_flip_vertically_on_load(true);

    // Load with requested channel count (0 = auto-detect)
    int width, height, channels;
    unsigned char* data = stbi_load(
        path.string().c_str(),
        &width, &height, &channels,
        desiredChannels
    );

    if (!data) {
        const char* reason = stbi_failure_reason();
        return Err<ImageData>("Failed to load image: " + std::string(reason ? reason : "unknown"));
    }

    // Use requested channels if specified, otherwise use native
    int actualChannels = (desiredChannels != 0) ? desiredChannels : channels;

    ImageData result;
    result.width = width;
    result.height = height;
    result.channels = actualChannels;

    size_t dataSize = static_cast<size_t>(width) * height * actualChannels;
    result.pixels.resize(dataSize);
    std::memcpy(result.pixels.data(), data, dataSize);

    stbi_image_free(data);

    return Ok(std::move(result));
}

bool ImageLoader::isSupported(const std::filesystem::path& path) {
    std::string ext = path.extension().string();

    // Convert to lowercase for comparison
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Supported formats
    static const std::vector<std::string> supportedExtensions = {
        ".png", ".jpg", ".jpeg", ".bmp", ".gif",
        ".tga", ".psd", ".hdr", ".pic", ".ppm", ".pgm"
    };

    for (const auto& supported : supportedExtensions) {
        if (ext == supported) {
            return true;
        }
    }

    return false;
}

} // namespace quadmapper
