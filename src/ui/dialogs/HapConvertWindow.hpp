#pragma once

// The Convert to HAP window (QM-082): one row per conversion with its progress and a
// Cancel button while it runs, then the outcome and "Replace in show". A plain
// window, not a modal: conversions run on their own threads and the show keeps
// playing and stays editable meanwhile.

#include "video/HapFrameEncoder.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace quadmapper {

class HapConversion;

class HapConvertWindow {
public:
    static constexpr const char* WINDOW_ID = "Convert to HAP##hapconvert";

    // A finished file to add to the media bin.
    using AddToBin = std::function<void(const std::filesystem::path&)>;
    // Move the show from original to converted; returns why not, or empty.
    using ReplaceInShow =
        std::function<std::string(const std::filesystem::path& original, const std::filesystem::path& converted)>;

    HapConvertWindow();
    ~HapConvertWindow();  // cancels running conversions and waits for their threads

    HapConvertWindow(const HapConvertWindow&) = delete;
    HapConvertWindow& operator=(const HapConvertWindow&) = delete;

    /// Start converting `source`, unless the same conversion is already running.
    void start(const std::filesystem::path& source, hap::HapVariant variant);
    bool isConverting(const std::filesystem::path& source, hap::HapVariant variant) const;

    /// Draw the window while it has conversions to show. While `showLocked` (the perform
    /// lock) "Replace in show" is disabled; Cancel and Dismiss stay usable, since they
    /// leave the show alone.
    void render(const AddToBin& addToBin, const ReplaceInShow& replaceInShow, bool showLocked);

private:
    struct Job {
        std::unique_ptr<HapConversion> conversion;
        bool addedToBin = false;
        bool replaced = false;
        std::string replaceError;
    };
    std::vector<Job> jobs_;
};

} // namespace quadmapper
