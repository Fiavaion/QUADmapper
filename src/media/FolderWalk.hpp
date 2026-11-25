#pragma once

#include "core/Logger.hpp"

#include <atomic>
#include <exception>
#include <filesystem>
#include <system_error>

namespace quadmapper {

// Visits every entry of a folder without throwing. The range-for form advances with the
// throwing operator++, so a folder on a drive unplugged mid-scan, or one holding a
// subfolder that may not be read, ended the app. Here a subfolder that may not be read
// is skipped, and any other read failure ends the walk. `stop` (optional) ends it early.
template <typename Iterator, typename Visit>
void walkFolder(const std::filesystem::path& folder, const std::atomic<bool>* stop, Visit&& visit) {
    std::error_code ec;
    for (Iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != Iterator(); it.increment(ec)) {
        if (stop && stop->load()) break;
        try {
            visit(*it);
        } catch (const std::exception& e) {
            // A name that is not valid UTF-16 has no UTF-8 form; the file is skipped.
            Logger::debug("Folder walk: skipped an entry ({})", e.what());
        }
    }
}

}  // namespace quadmapper
