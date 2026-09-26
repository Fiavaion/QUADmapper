#include "ThumbnailCache.hpp"
#include "ImageLoader.hpp"
#include "MediaLibrary.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace quadmapper {

ThumbnailCache::ThumbnailCache() {
    // Ensure cache directory exists
    std::error_code ec;
    std::filesystem::create_directories(getCacheDir(), ec);
}

ThumbnailCache::~ThumbnailCache() {
    clearMemoryCache();
}

std::filesystem::path ThumbnailCache::getCacheDir() {
    const char* appData = std::getenv("APPDATA");
    if (appData) {
        return std::filesystem::path(appData) / "QUADmapper" / "thumbnails";
    }
    // Fallback to temp directory
    return std::filesystem::temp_directory_path() / "QUADmapper" / "thumbnails";
}

std::filesystem::path ThumbnailCache::getCachePath(const std::filesystem::path& filePath) const {
    // Create a hash from the full path for a unique filename
    std::hash<std::string> hasher;
    size_t hash = hasher(filePath.string());
    // "v2": thumbnails are now always stored top row first; older ones could be flipped.
    return getCacheDir() / (std::to_string(hash) + ".v2.thumb");
}

bool ThumbnailCache::isCacheValid(const std::filesystem::path& filePath,
                                   const std::filesystem::path& cachePath) const {
    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec)) {
        return false;
    }
    if (!std::filesystem::exists(filePath, ec)) {
        return false;
    }

    // Check if source is newer than cache
    auto cacheTime = std::filesystem::last_write_time(cachePath, ec);
    if (ec) return false;
    auto sourceTime = std::filesystem::last_write_time(filePath, ec);
    if (ec) return false;

    return sourceTime <= cacheTime;
}

GLuint ThumbnailCache::getThumbnail(const std::filesystem::path& filePath, int maxSize) {
    std::lock_guard<std::mutex> lock(cacheMutex_);

    std::string pathKey = filePath.string();

    // Check memory cache first
    auto it = textureCache_.find(pathKey);
    if (it != textureCache_.end()) {
        return it->second;
    }

    // Check disk cache
    std::filesystem::path cachePath = getCachePath(filePath);
    if (isCacheValid(filePath, cachePath)) {
        GLuint texId = loadCachedThumbnail(cachePath);
        if (texId != 0) {
            textureCache_[pathKey] = texId;
            return texId;
        }
    }

    // Generate new thumbnail
    GLuint texId = generateThumbnail(filePath, maxSize);
    if (texId != 0) {
        textureCache_[pathKey] = texId;
    }
    return texId;
}

bool ThumbnailCache::hasThumbnail(const std::filesystem::path& filePath) const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return textureCache_.find(filePath.string()) != textureCache_.end();
}

void ThumbnailCache::invalidate(const std::filesystem::path& filePath) {
    std::lock_guard<std::mutex> lock(cacheMutex_);

    std::string pathKey = filePath.string();
    auto it = textureCache_.find(pathKey);
    if (it != textureCache_.end()) {
        glDeleteTextures(1, &it->second);
        textureCache_.erase(it);
    }

    // Also remove disk cache
    std::error_code ec;
    std::filesystem::remove(getCachePath(filePath), ec);
}

void ThumbnailCache::clearMemoryCache() {
    std::lock_guard<std::mutex> lock(cacheMutex_);

    for (auto& [path, texId] : textureCache_) {
        glDeleteTextures(1, &texId);
    }
    textureCache_.clear();
}

void ThumbnailCache::clearAllCaches() {
    clearMemoryCache();

    // Clear disk cache
    std::error_code ec;
    std::filesystem::path cacheDir = getCacheDir();
    if (std::filesystem::exists(cacheDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec)) {
            if (entry.path().extension() == ".thumb") {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }
}

size_t ThumbnailCache::getDiskCacheSize() const {
    size_t totalSize = 0;
    std::error_code ec;
    std::filesystem::path cacheDir = getCacheDir();

    if (std::filesystem::exists(cacheDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".thumb") {
                totalSize += static_cast<size_t>(entry.file_size(ec));
            }
        }
    }
    return totalSize;
}

size_t ThumbnailCache::getMemoryCacheCount() const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return textureCache_.size();
}

GLuint ThumbnailCache::generateThumbnail(const std::filesystem::path& filePath, int maxSize) {
    // Check if it's a supported image format
    std::string ext = filePath.extension().string();
    if (!MediaLibrary::isExtensionSupported(ext)) {
        return 0;
    }

    // Load through ImageLoader so every image format (TIFF included) thumbnails, always
    // as RGBA stored bottom row first.
    auto loaded = ImageLoader::loadWithChannels(filePath, 4);
    if (!loaded.isSuccess()) {
        Logger::debug("ThumbnailCache: Failed to load image: {}", filePath.filename().string());
        return 0;
    }
    const ImageData& image = loaded.value();
    const int width = image.width;
    const int height = image.height;
    const uint8_t* data = image.pixels.data();

    // Calculate thumbnail dimensions maintaining aspect ratio
    int thumbWidth, thumbHeight;
    if (width > height) {
        thumbWidth = maxSize;
        thumbHeight = (height * maxSize) / width;
    } else {
        thumbHeight = maxSize;
        thumbWidth = (width * maxSize) / height;
    }

    // Ensure minimum dimensions
    thumbWidth = std::max(1, thumbWidth);
    thumbHeight = std::max(1, thumbHeight);

    // Simple nearest-neighbor resize. The source is bottom row first but the bin draws
    // thumbnails with UV (0,0) at the top, so thumbnail rows are written top row first.
    unsigned char* thumbData = new unsigned char[thumbWidth * thumbHeight * 4];
    for (int y = 0; y < thumbHeight; ++y) {
        for (int x = 0; x < thumbWidth; ++x) {
            int srcX = (x * width) / thumbWidth;
            int srcY = height - 1 - (y * height) / thumbHeight;
            int srcIdx = (srcY * width + srcX) * 4;
            int dstIdx = (y * thumbWidth + x) * 4;

            thumbData[dstIdx + 0] = data[srcIdx + 0];
            thumbData[dstIdx + 1] = data[srcIdx + 1];
            thumbData[dstIdx + 2] = data[srcIdx + 2];
            thumbData[dstIdx + 3] = data[srcIdx + 3];
        }
    }

    // Create OpenGL texture
    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, thumbWidth, thumbHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, thumbData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Save to disk cache
    saveThumbnailToCache(getCachePath(filePath), thumbData, thumbWidth, thumbHeight);

    delete[] thumbData;

    Logger::debug("ThumbnailCache: Generated thumbnail for: {} ({}x{})",
                 filePath.filename().string(), thumbWidth, thumbHeight);

    return texId;
}

GLuint ThumbnailCache::loadCachedThumbnail(const std::filesystem::path& cachePath) {
    // Read thumbnail from disk cache
    std::ifstream file(cachePath, std::ios::binary);
    if (!file) {
        return 0;
    }

    // Read header: width (4 bytes), height (4 bytes)
    int width = 0, height = 0;
    file.read(reinterpret_cast<char*>(&width), sizeof(int));
    file.read(reinterpret_cast<char*>(&height), sizeof(int));

    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        return 0;
    }

    // Read pixel data
    size_t dataSize = static_cast<size_t>(width) * height * 4;
    std::vector<unsigned char> data(dataSize);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(dataSize));

    if (!file || file.gcount() != static_cast<std::streamsize>(dataSize)) {
        return 0;
    }

    // Create OpenGL texture
    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return texId;
}

void ThumbnailCache::saveThumbnailToCache(const std::filesystem::path& cachePath,
                                           const unsigned char* data,
                                           int width, int height) {
    // Ensure cache directory exists
    std::error_code ec;
    std::filesystem::create_directories(cachePath.parent_path(), ec);

    // Write thumbnail to disk in simple binary format:
    // [width: 4 bytes][height: 4 bytes][RGBA data: width*height*4 bytes]
    std::ofstream file(cachePath, std::ios::binary);
    if (!file) {
        Logger::debug("ThumbnailCache: Failed to create cache file: {}", cachePath.filename().string());
        return;
    }

    file.write(reinterpret_cast<const char*>(&width), sizeof(int));
    file.write(reinterpret_cast<const char*>(&height), sizeof(int));
    file.write(reinterpret_cast<const char*>(data),
               static_cast<std::streamsize>(width * height * 4));
}

} // namespace quadmapper
