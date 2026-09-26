#pragma once

// Finding moved media files (PRD §4.1.8). When a project opens and a media or mesh
// file is gone, the user points at its new location, or at a folder to search; these
// helpers decide which file on disk replaces which missing path. Pure filesystem
// logic with no GL or UI, so the matching rules are unit-tested on their own.

#include <atomic>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace quadmapper::relocate {

// Compare file names the way the file system does: case-insensitively on Windows,
// exactly elsewhere.
inline std::wstring nameKey(std::wstring name) {
#ifdef _WIN32
    for (wchar_t& c : name) c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
#endif
    return name;
}

// Every regular file below a folder, by file name.
struct FolderIndex {
    std::unordered_map<std::wstring, std::filesystem::path> byName;  // nameKey(file name) -> file
    bool truncated = false;  // stopped early: cancelled or too many entries
};

// A search of a whole drive stops here instead of running for minutes.
inline constexpr std::size_t MAX_INDEXED_ENTRIES = 200000;

// Index root recursively (symlinked folders are not followed; unreadable folders are
// skipped). When several files share a name the first match wins, defined as: the
// shallowest file (one directly in root beats one in a subfolder), then among equally
// deep files the case-insensitively smallest full path. The choice never depends on
// the order the file system happens to list entries in.
inline FolderIndex indexFolder(const std::filesystem::path& root,
                               const std::atomic<bool>* cancel = nullptr,
                               std::size_t maxEntries = MAX_INDEXED_ENTRIES) {
    namespace fs = std::filesystem;
    FolderIndex index;
    std::unordered_map<std::wstring, std::pair<int, std::wstring>> rank;  // name -> (depth, path key)
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    std::size_t seen = 0;
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if ((cancel && cancel->load()) || ++seen > maxEntries) {
            index.truncated = true;
            break;
        }
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) continue;
        const fs::path& file = it->path();
        const std::wstring key = nameKey(file.filename().wstring());
        std::pair<int, std::wstring> order{it.depth(), nameKey(file.generic_wstring())};
        auto found = rank.find(key);
        if (found == rank.end() || order < found->second) {
            rank[key] = std::move(order);
            index.byName[key] = file;
        }
    }
    return index;
}

// The indexed file with the same name as missing, or empty.
inline std::filesystem::path findInIndex(const FolderIndex& index,
                                         const std::filesystem::path& missing) {
    auto it = index.byName.find(nameKey(missing.filename().wstring()));
    return it != index.byName.end() ? it->second : std::filesystem::path();
}

// A file relocated into dir suggests its missing neighbours moved there too: the file
// in dir with missing's name, or empty.
inline std::filesystem::path findSibling(const std::filesystem::path& dir,
                                         const std::filesystem::path& missing) {
    if (dir.empty() || missing.filename().empty()) return {};
    const std::filesystem::path candidate = dir / missing.filename();
    std::error_code ec;
    return std::filesystem::is_regular_file(candidate, ec) ? candidate : std::filesystem::path();
}

}  // namespace quadmapper::relocate
