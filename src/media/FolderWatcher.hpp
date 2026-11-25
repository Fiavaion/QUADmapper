#pragma once

#include <filesystem>
#include <functional>
#include <thread>
#include <atomic>
#include <string>
#include <unordered_map>
#include <mutex>

namespace quadmapper {

/**
 * @brief Watches a folder for file changes using polling.
 *
 * Monitors a directory for new and modified files on a background thread.
 * Calls a callback when changes are detected.
 */
class FolderWatcher {
public:
    /// Callback signature: (path, isNewFile)
    using FileCallback = std::function<void(const std::filesystem::path&, bool isNew)>;

    FolderWatcher();
    ~FolderWatcher();

    // Non-copyable
    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    /**
     * @brief Start watching a folder for changes.
     * @param folder The directory path to watch
     * @param recursive Whether to watch subdirectories
     *
     * Stops any existing watch before starting the new one.
     */
    void watch(const std::filesystem::path& folder, bool recursive = false);

    /**
     * @brief Stop watching the folder.
     */
    void stop();

    /**
     * @brief Set the callback for file changes.
     * @param callback Function called when files are added or modified
     *
     * The callback is invoked from the watcher thread.
     */
    void setCallback(FileCallback callback) { callback_ = std::move(callback); }

private:
    void watchThread();
    void buildInitialCache();

    template<typename Iterator>
    void scanDirectory(Iterator iterator);

    std::filesystem::path folder_;
    bool recursive_ = false;
    std::atomic<bool> watching_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread watchThread_;
    FileCallback callback_;
    int pollingIntervalMs_ = 2000;

    // Cache of known files and their modification times
    std::unordered_map<std::string, std::filesystem::file_time_type> fileCache_;
    std::mutex cacheMutex_;
};

} // namespace quadmapper
