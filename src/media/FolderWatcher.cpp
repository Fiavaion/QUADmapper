#include "FolderWatcher.hpp"
#include "FolderWalk.hpp"
#include "MediaLibrary.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include <chrono>

namespace quadmapper {

FolderWatcher::FolderWatcher() = default;

FolderWatcher::~FolderWatcher() {
    stop();
}

void FolderWatcher::watch(const std::filesystem::path& folder, bool recursive) {
    // Stop any existing watcher first
    stop();

    std::error_code ec;
    if (!std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec)) {
        Logger::warning("FolderWatcher: Cannot watch '{}' - not a valid directory", utf8FromPath(folder));
        return;
    }

    folder_ = folder;
    recursive_ = recursive;
    stopRequested_ = false;

    // Build initial cache of files
    buildInitialCache();

    // Start watcher thread
    watching_ = true;
    watchThread_ = std::thread(&FolderWatcher::watchThread, this);

    Logger::info("FolderWatcher: Started watching '{}' (recursive: {})",
                 utf8FromPath(folder), recursive ? "yes" : "no");
}

void FolderWatcher::stop() {
    if (!watching_.load()) {
        return;
    }

    stopRequested_ = true;
    watching_ = false;

    if (watchThread_.joinable()) {
        watchThread_.join();
    }

    std::lock_guard<std::mutex> lock(cacheMutex_);
    fileCache_.clear();

    Logger::debug("FolderWatcher: Stopped watching");
}

void FolderWatcher::buildInitialCache() {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    fileCache_.clear();

    std::error_code ec;
    if (!std::filesystem::exists(folder_, ec)) {
        return;
    }

    auto cacheEntry = [this](const std::filesystem::directory_entry& entry) {
        std::error_code ec;
        if (!entry.is_regular_file(ec) || ec) {
            return;
        }

        // Only cache supported media files
        std::string ext = utf8FromPath(entry.path().extension());
        if (!MediaLibrary::isExtensionSupported(ext)) {
            return;
        }

        auto modTime = std::filesystem::last_write_time(entry.path(), ec);
        if (!ec) {
            fileCache_[utf8FromPath(entry.path())] = modTime;
        }
    };

    if (recursive_) {
        walkFolder<std::filesystem::recursive_directory_iterator>(folder_, nullptr, cacheEntry);
    } else {
        walkFolder<std::filesystem::directory_iterator>(folder_, nullptr, cacheEntry);
    }

    Logger::debug("FolderWatcher: Cached {} existing files", fileCache_.size());
}

void FolderWatcher::watchThread() {
    while (!stopRequested_.load()) {
        // Sleep for polling interval
        for (int i = 0; i < pollingIntervalMs_ / 100 && !stopRequested_.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (stopRequested_.load()) {
            break;
        }

        std::error_code ec;
        if (!std::filesystem::exists(folder_, ec)) {
            continue;
        }

        // Scan for new/modified files
        auto processEntry = [this](const std::filesystem::directory_entry& entry) {
            std::error_code ec;
            if (!entry.is_regular_file(ec) || ec) {
                return;
            }

            // Only watch supported media files
            std::string ext = utf8FromPath(entry.path().extension());
            if (!MediaLibrary::isExtensionSupported(ext)) {
                return;
            }

            std::string pathStr = utf8FromPath(entry.path());
            auto modTime = std::filesystem::last_write_time(entry.path(), ec);
            if (ec) {
                return;
            }

            std::lock_guard<std::mutex> lock(cacheMutex_);
            auto it = fileCache_.find(pathStr);

            if (it == fileCache_.end()) {
                // New file
                fileCache_[pathStr] = modTime;
                if (callback_) {
                    callback_(entry.path(), true);
                }
            } else if (it->second != modTime) {
                // Modified file
                it->second = modTime;
                if (callback_) {
                    callback_(entry.path(), false);
                }
            }
        };

        if (recursive_) {
            walkFolder<std::filesystem::recursive_directory_iterator>(folder_, &stopRequested_, processEntry);
        } else {
            walkFolder<std::filesystem::directory_iterator>(folder_, &stopRequested_, processEntry);
        }
    }
}

} // namespace quadmapper
