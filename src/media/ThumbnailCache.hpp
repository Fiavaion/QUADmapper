#pragma once

#include <filesystem>
#include <unordered_map>
#include <mutex>
#include <glad/glad.h>

namespace quadmapper {

/**
 * @brief Manages thumbnail generation and caching for media files.
 *
 * Generates thumbnails on-demand and caches them both in memory (OpenGL textures)
 * and on disk (JPEG files in %APPDATA%\QUADmapper\thumbnails).
 */
class ThumbnailCache {
public:
    ThumbnailCache();
    ~ThumbnailCache();

    // Non-copyable
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    /**
     * @brief Get or generate a thumbnail texture for a media file.
     * @param filePath Path to the media file
     * @param maxSize Maximum dimension for the thumbnail (default 128)
     * @return OpenGL texture ID, or 0 if generation failed
     *
     * The thumbnail is cached both in memory and on disk. Subsequent calls
     * with the same file path return the cached texture.
     */
    GLuint getThumbnail(const std::filesystem::path& filePath, int maxSize = 128);

    /**
     * @brief Check if a thumbnail exists in the cache.
     * @param filePath Path to the media file
     * @return true if thumbnail is cached in memory
     */
    bool hasThumbnail(const std::filesystem::path& filePath) const;

    /**
     * @brief Invalidate and regenerate a thumbnail.
     * @param filePath Path to the media file
     *
     * Removes the cached thumbnail so the next getThumbnail() call
     * will regenerate it.
     */
    void invalidate(const std::filesystem::path& filePath);

    /**
     * @brief Clear all cached thumbnails from memory.
     *
     * Deletes all OpenGL textures. Disk cache is preserved.
     */
    void clearMemoryCache();

    /**
     * @brief Clear both memory and disk caches.
     */
    void clearAllCaches();

    /**
     * @brief Get the disk cache directory path.
     * @return Path to %APPDATA%\QUADmapper\thumbnails
     */
    static std::filesystem::path getCacheDir();

    /**
     * @brief Get the total size of the disk cache in bytes.
     */
    size_t getDiskCacheSize() const;

    /**
     * @brief Get the number of thumbnails in memory cache.
     */
    size_t getMemoryCacheCount() const;

private:
    /// Generate a new thumbnail from a source image
    GLuint generateThumbnail(const std::filesystem::path& filePath, int maxSize);

    /// Load a thumbnail from disk cache
    GLuint loadCachedThumbnail(const std::filesystem::path& cachePath);

    /// Save thumbnail data to disk cache
    void saveThumbnailToCache(const std::filesystem::path& cachePath,
                              const unsigned char* data, int width, int height);

    /// Get the cache file path for a source file
    std::filesystem::path getCachePath(const std::filesystem::path& filePath) const;

    /// Check if the cache file is newer than the source file
    bool isCacheValid(const std::filesystem::path& filePath,
                      const std::filesystem::path& cachePath) const;

    /// In-memory texture cache: file path -> GL texture ID
    std::unordered_map<std::string, GLuint> textureCache_;
    mutable std::mutex cacheMutex_;

    /// Default thumbnail size
    int defaultMaxSize_ = 128;
};

} // namespace quadmapper
