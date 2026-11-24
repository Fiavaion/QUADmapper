#include "MediaLibrary.hpp"
#include "FolderWalk.hpp"
#include "core/Logger.hpp"
#include <algorithm>
#include <cctype>

namespace quadmapper {

// Supported file extensions
// Matches ImageLoader::isSupported.
static const std::vector<std::string> IMAGE_EXTENSIONS = {
    ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga", ".psd", ".hdr",
    ".pic", ".ppm", ".pgm", ".tif", ".tiff"
};

static const std::vector<std::string> VIDEO_EXTENSIONS = {
    ".mp4", ".mov", ".avi", ".mkv", ".webm", ".wmv", ".flv", ".m4v"
};

MediaItem* MediaLibrary::addItem(const std::filesystem::path& path) {
    if (hasPath(path)) {
        return nullptr;
    }

    const std::optional<MediaType> type = getTypeFromExtension(utf8FromPath(path.extension()));
    if (!type) {
        Logger::warning("Not an image or video file QUADmapper can open: {}", utf8FromPath(path));
        return nullptr;
    }

    auto item = std::make_unique<MediaItem>();
    item->setPath(path);
    item->setType(*type);
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

        std::string ext = utf8FromPath(entry.path().extension());
        if (isExtensionSupported(ext) && !hasPath(entry.path())) {
            if (addItem(entry.path())) {
                count++;
            }
        }
    };

    if (recursive) {
        walkFolder<std::filesystem::recursive_directory_iterator>(folder, nullptr, processEntry);
    } else {
        walkFolder<std::filesystem::directory_iterator>(folder, nullptr, processEntry);
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
    if (!ext.empty() && ext[0] != '.') {
        ext = "." + ext;
    }
    // Convert to lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& imgExt : IMAGE_EXTENSIONS) {
        if (ext == imgExt) return true;
    }
    for (const auto& vidExt : VIDEO_EXTENSIONS) {
        if (ext == vidExt) return true;
    }
    return false;
}

std::optional<MediaType> MediaLibrary::getTypeFromExtension(const std::string& extension) {
    std::string ext = extension;
    if (!ext.empty() && ext[0] != '.') {
        ext = "." + ext;
    }
    // Convert to lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& vidExt : VIDEO_EXTENSIONS) {
        if (ext == vidExt) return MediaType::VIDEO;
    }
    for (const auto& imgExt : IMAGE_EXTENSIONS) {
        if (ext == imgExt) return MediaType::IMAGE;
    }
    return std::nullopt;
}

} // namespace quadmapper
