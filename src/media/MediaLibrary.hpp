#pragma once

#include "MediaItem.hpp"
#include <vector>
#include <memory>
#include <string>

namespace quadmapper {

/**
 * MediaLibrary manages a collection of media items (images and videos).
 * It provides import, removal, and lookup functionality for the media bin.
 */
class MediaLibrary {
public:
    MediaLibrary() = default;

    /**
     * Add a media item from a file path.
     * @param path Path to the media file
     * @return Pointer to the created item, or nullptr if import failed
     */
    MediaItem* addItem(const std::filesystem::path& path);

    /**
     * Add a pre-created media item (e.g., procedural sources).
     * @param item The item to add (ownership transferred)
     * @return Pointer to the added item
     */
    MediaItem* addItem(std::unique_ptr<MediaItem> item);

    /**
     * Remove a media item by ID.
     * @param id The UUID of the item to remove
     */
    void removeItem(const UUID& id);

    /**
     * Get a media item by ID.
     * @param id The UUID of the item
     * @return Pointer to the item, or nullptr if not found
     */
    MediaItem* getItem(const UUID& id);

    /**
     * Get a media item by ID (const version).
     */
    const MediaItem* getItem(const UUID& id) const;

    /**
     * Get the number of items in the library.
     */
    size_t getItemCount() const { return items_.size(); }

    /**
     * Check if the library is empty.
     */
    bool isEmpty() const { return items_.empty(); }

    /**
     * Get all items in the library.
     */
    const std::vector<std::unique_ptr<MediaItem>>& getItems() const { return items_; }

    /**
     * Clear all items from the library.
     */
    void clear() { items_.clear(); }

    /**
     * Scan a directory and import all supported media files.
     * @param folder Path to the folder to scan
     * @param recursive Whether to scan subdirectories
     * @return Number of files imported
     */
    int scanFolder(const std::filesystem::path& folder, bool recursive = false);

    /**
     * Check if a file path is already in the library.
     * @param path Path to check
     * @return true if the path is already imported
     */
    bool hasPath(const std::filesystem::path& path) const;

    /**
     * Get supported image file extensions.
     */
    static const std::vector<std::string>& getSupportedImageExtensions();

    /**
     * Get supported video file extensions.
     */
    static const std::vector<std::string>& getSupportedVideoExtensions();

    /**
     * Check if a file extension is supported.
     * @param extension The extension (with or without dot)
     * @return true if the extension is supported
     */
    static bool isExtensionSupported(const std::string& extension);

    /**
     * Determine media type from file extension.
     * @param extension The extension (with or without dot)
     * @return The media type, or IMAGE as default
     */
    static MediaType getTypeFromExtension(const std::string& extension);

private:
    std::vector<std::unique_ptr<MediaItem>> items_;
};

} // namespace quadmapper
