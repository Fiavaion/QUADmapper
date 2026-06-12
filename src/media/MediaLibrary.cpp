#include "MediaLibrary.hpp"
#include <algorithm>
#include <cctype>

namespace quadmapper {

// Supported file extensions
static const std::vector<std::string> IMAGE_EXTENSIONS = {
    ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga", ".psd", ".hdr"
};

static const std::vector<std::string> VIDEO_EXTENSIONS = {
    ".mp4", ".mov", ".avi", ".mkv", ".webm", ".wmv", ".flv", ".m4v"
};

MediaItem* MediaLibrary::addItem(const std::filesystem::path& path) {
    // Check if path already exists
    if (hasPath(path)) {
        return nullptr;
    }

    // Determine media type from extension
    std::string ext = path.extension().string();
    MediaType type = getTypeFromExtension(ext);

    auto item = std::make_unique<MediaItem>();
    item->setPath(path);
    item->setType(type);
    MediaItem* ptr = item.get();
    items_.push_back(std::move(item));
    return ptr;
}

MediaItem* MediaLibrary::addItem(std::unique_ptr<MediaItem> item) {
    if (!item) {
        return nullptr;
    }
    MediaItem* ptr = item.get();
    items_.push_back(std::move(item));
    return ptr;
}

void MediaLibrary::removeItem(const UUID& id) {
    auto it = std::find_if(items_.begin(), items_.end(),
        [&](const auto& i) { return i->getId() == id; });
    if (it != items_.end()) {
        items_.erase(it);
    }
}

MediaItem* MediaLibrary::getItem(const UUID& id) {
    auto it = std::find_if(items_.begin(), items_.end(),
        [&](const auto& i) { return i->getId() == id; });
    return it != items_.end() ? it->get() : nullptr;
}

const MediaItem* MediaLibrary::getItem(const UUID& id) const {
    auto it = std::find_if(items_.begin(), items_.end(),
        [&](const auto& i) { return i->getId() == id; });
    return it != items_.end() ? it->get() : nullptr;
}

bool MediaLibrary::hasPath(const std::filesystem::path& path) const {
    return std::find_if(items_.begin(), items_.end(),
        [&](const auto& i) { return i->getPath() == path; }) != items_.end();
}

int MediaLibrary::scanFolder(const std::filesystem::path& folder, bool recursive) {
    int count = 0;

    // Check if folder exists and is a directory
    std::error_code ec;
    if (!std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec)) {
        return 0;
    }

    // Helper lambda to process a directory entry
    auto processEntry = [this, &count](const std::filesystem::directory_entry& entry) {
        std::error_code ec;
        if (!entry.is_regular_file(ec) || ec) {
            return;
        }

        std::string ext = entry.path().extension().string();
        if (isExtensionSupported(ext) && !hasPath(entry.path())) {
            if (addItem(entry.path())) {
                count++;
            }
        }
    };

    // Scan directory
    if (recursive) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ec)) {
            if (ec) break;
            processEntry(entry);
        }
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            if (ec) break;
            processEntry(entry);
        }
    }

    return count;
}

const std::vector<std::string>& MediaLibrary::getSupportedImageExtensions() {
    return IMAGE_EXTENSIONS;
}

const std::vector<std::string>& MediaLibrary::getSupportedVideoExtensions() {
    return VIDEO_EXTENSIONS;
}

bool MediaLibrary::isExtensionSupported(const std::string& extension) {
    std::string ext = extension;
    // Add dot if not present
    if (!ext.empty() && ext[0] != '.') {
        ext = "." + ext;
    }
    // Convert to lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Check image extensions
    for (const auto& imgExt : IMAGE_EXTENSIONS) {
        if (ext == imgExt) return true;
    }
    // Check video extensions
    for (const auto& vidExt : VIDEO_EXTENSIONS) {
        if (ext == vidExt) return true;
    }
    return false;
}

MediaType MediaLibrary::getTypeFromExtension(const std::string& extension) {
    std::string ext = extension;
    // Add dot if not present
    if (!ext.empty() && ext[0] != '.') {
        ext = "." + ext;
    }
    // Convert to lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Check video extensions
    for (const auto& vidExt : VIDEO_EXTENSIONS) {
        if (ext == vidExt) return MediaType::VIDEO;
    }
    // Default to image
    return MediaType::IMAGE;
}

} // namespace quadmapper
