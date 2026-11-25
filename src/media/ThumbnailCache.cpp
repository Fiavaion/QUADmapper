#include "ThumbnailCache.hpp"
#include "ImageLoader.hpp"
#include "MediaLibrary.hpp"
#include "core/FileSystem.hpp"
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
    // Wide: a narrow getenv narrows a non-ASCII user folder to the ANSI code page.
    wchar_t* appData = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&appData, &length, L"APPDATA") == 0 && appData) {
        const std::filesystem::path dir = std::filesystem::path(appData) / "QUADmapper" / "thumbnails";
        free(appData);
        return dir;
    }
    free(appData);
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "QUADmapper" / "thumbnails";
}

std::filesystem::path ThumbnailCache::getCachePath(const std::filesystem::path& filePath) const {
    // Create a hash from the full path for a unique filename
    std::hash<std::string> hasher;
    size_t hash = hasher(utf8FromPath(filePath));
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

    auto cacheTime = std::filesystem::last_write_time(cachePath, ec);
    if (ec) return false;
    auto sourceTime = std::filesystem::last_write_time(filePath, ec);
    if (ec) return false;

    return sourceTime <= cacheTime;
}

namespace {
// A file that cannot be stat'ed (missing) reads as the minimum time, so it too
// counts as unchanged until it appears.
std::filesystem::file_time_type lastWriteTimeOrMin(const std::filesystem::path& path) {
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type::min() : writeTime;
}
}  // namespace

bool ThumbnailCache::failedUnchanged(const std::filesystem::path& filePath,
                                     const std::string& pathKey) const {
    auto failed = failedFiles_.find(pathKey);
    if (failed == failedFiles_.end()) return false;
    return lastWriteTimeOrMin(filePath) == failed->second;
}

bool ThumbnailCache::needsGeneration(const std::filesystem::path& filePath) const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    const std::string pathKey = utf8FromPath(filePath);
    return textureCache_.find(pathKey) == textureCache_.end() && !failedUnchanged(filePath, pathKey);
}

GLuint ThumbnailCache::getThumbnail(const std::filesystem::path& filePath, int maxSize) {
    std::lock_guard<std::mutex> lock(cacheMutex_);

    std::string pathKey = utf8FromPath(filePath);

    auto it = textureCache_.find(pathKey);
    if (it != textureCache_.end()) {
        return it->second;
    }

    // A file that failed and has not changed since is not read or decoded again.
    if (failedUnchanged(filePath, pathKey)) return 0;

    std::filesystem::path cachePath = getCachePath(filePath);
    if (isCacheValid(filePath, cachePath)) {
        GLuint texId = loadCachedThumbnail(cachePath);
        if (texId != 0) {
            textureCache_[pathKey] = texId;
            return texId;
        }
    }

    GLuint texId = generateThumbnail(filePath, maxSize);
    if (texId != 0) {
        textureCache_[pathKey] = texId;
        failedFiles_.erase(pathKey);
    } else {
        failedFiles_[pathKey] = lastWriteTimeOrMin(filePath);
    }
    return texId;
}

void ThumbnailCache::invalidate(const std::filesystem::path& filePath) {
    std::lock_guard<std::mutex> lock(cacheMutex_);

    std::string pathKey = utf8FromPath(filePath);
    auto it = textureCache_.find(pathKey);
    if (it != textureCache_.end()) {
        glDeleteTextures(1, &it->second);
        textureCache_.erase(it);
    }
    failedFiles_.erase(pathKey);

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
    failedFiles_.clear();
}

GLuint ThumbnailCache::generateThumbnail(const std::filesystem::path& filePath, int maxSize) {
    std::string ext = utf8FromPath(filePath.extension());
    if (!MediaLibrary::isExtensionSupported(ext)) {
        return 0;
    }

    // Load through ImageLoader so every image format (TIFF included) thumbnails, always
    // as RGBA stored top row first.
    auto loaded = ImageLoader::loadWithChannels(filePath, 4);
    if (!loaded.isSuccess()) {
        Logger::debug("ThumbnailCache: Failed to load image: {}", utf8FromPath(filePath.filename()));
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

    // Simple nearest-neighbor resize, top row first (the bin draws UV (0,0) at the top).
    std::vector<unsigned char> thumbData(static_cast<size_t>(thumbWidth) * thumbHeight * 4);
    for (int y = 0; y < thumbHeight; ++y) {
        for (int x = 0; x < thumbWidth; ++x) {
            int srcX = (x * width) / thumbWidth;
            int srcY = (y * height) / thumbHeight;
            int srcIdx = (srcY * width + srcX) * 4;
            int dstIdx = (y * thumbWidth + x) * 4;

            thumbData[dstIdx + 0] = data[srcIdx + 0];
            thumbData[dstIdx + 1] = data[srcIdx + 1];
            thumbData[dstIdx + 2] = data[srcIdx + 2];
            thumbData[dstIdx + 3] = data[srcIdx + 3];
        }
    }

    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, thumbWidth, thumbHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, thumbData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    saveThumbnailToCache(getCachePath(filePath), thumbData.data(), thumbWidth, thumbHeight);

    Logger::debug("ThumbnailCache: Generated thumbnail for: {} ({}x{})",
                 utf8FromPath(filePath.filename()), thumbWidth, thumbHeight);

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
        Logger::debug("ThumbnailCache: Failed to create cache file: {}", utf8FromPath(cachePath.filename()));
        return;
    }

    file.write(reinterpret_cast<const char*>(&width), sizeof(int));
    file.write(reinterpret_cast<const char*>(&height), sizeof(int));
    file.write(reinterpret_cast<const char*>(data),
               static_cast<std::streamsize>(width * height * 4));
    file.close();
    if (!file) {
        Logger::debug("ThumbnailCache: Failed to write cache file: {}", utf8FromPath(cachePath.filename()));
        std::filesystem::remove(cachePath, ec);
    }
}

} // namespace quadmapper
