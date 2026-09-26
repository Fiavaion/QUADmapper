#pragma once

#include "Result.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace quadmapper {

// Every path held in a std::string (project model, .qmap, preferences, GLFW drops,
// dialogs, log lines, FFmpeg URLs) is UTF-8. On Windows the narrow std::filesystem
// constructor and path::string() use the ANSI code page instead, which garbles
// accented folder names and throws on Cyrillic or Japanese ones; convert through these.
std::filesystem::path pathFromUtf8(const std::string& utf8);
std::string utf8FromPath(const std::filesystem::path& path);

class FileSystem {
public:
    // Read entire file to string
    static Result<std::string, Error> readFile(const std::filesystem::path& path);

    // Write string to file
    static Result<void, Error> writeFile(const std::filesystem::path& path, const std::string& content);

    // Crash-safe replace (ADR-046): write <path>.tmp, flush it to disk, copy any
    // existing <path> to <path>.bak (one rolling backup), then atomically move the
    // temp over <path>. On any failure returns Err and leaves <path> untouched.
    static Result<void, Error> writeFileAtomic(const std::filesystem::path& path,
                                               const std::string& content);

    // Read binary file
    static Result<std::vector<uint8_t>, Error> readBinaryFile(const std::filesystem::path& path);

    // Write binary file
    static Result<void, Error> writeBinaryFile(const std::filesystem::path& path,
                                               const std::vector<uint8_t>& data);

    // Check if file exists
    static bool exists(const std::filesystem::path& path);

    // Check if path is directory
    static bool isDirectory(const std::filesystem::path& path);

    // Create directory (and parents)
    static Result<void, Error> createDirectory(const std::filesystem::path& path);

    // Make path relative to base
    static std::filesystem::path makeRelative(const std::filesystem::path& path,
                                              const std::filesystem::path& base);

    // Resolve relative path from base
    static std::filesystem::path resolveRelative(const std::filesystem::path& relativePath,
                                                 const std::filesystem::path& base);
};

} // namespace quadmapper
