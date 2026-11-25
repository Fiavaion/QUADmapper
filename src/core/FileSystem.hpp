#pragma once

#include "Result.hpp"
#include <cstdint>
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

// Where the program and the user's files live. None of these depend on the folder the
// app was started from: a shortcut, a double-clicked .qmap and an install under
// Program Files each start it somewhere else, and Program Files cannot be written to.
std::filesystem::path executableDir();                              // folder of the .exe
std::filesystem::path resourcePath(const std::string& relative);    // <exe folder>/resources/<relative>
//
//                     Windows                           macOS
// userConfigDir()     %APPDATA%\QUADmapper              ~/Library/Application Support/QUADmapper
// userLocalDir()      %LOCALAPPDATA%\QUADmapper         ~/Library/Caches/QUADmapper
// userLogDir()        %LOCALAPPDATA%\QUADmapper\logs    ~/Library/Logs/QUADmapper
// userRecoveryDir()   %LOCALAPPDATA%\QUADmapper\recovery  ~/Library/Application Support/QUADmapper/recovery
std::filesystem::path userConfigDir();    // settings that follow the user
std::filesystem::path userLocalDir();     // this machine only: crash dumps, caches (macOS may empty it)
std::filesystem::path userLogDir();       // the log file
std::filesystem::path userRecoveryDir();  // autosave and crash copies of untitled shows

class FileSystem {
public:
    // Read entire file to string. A file over maxBytes, or one there is no memory
    // for, is an error and is not read.
    static Result<std::string, Error> readFile(const std::filesystem::path& path,
                                               size_t maxBytes = SIZE_MAX);

    // Write string to file
    static Result<void, Error> writeFile(const std::filesystem::path& path, const std::string& content);

    // Crash-safe replace (ADR-046): write <path>.tmp, flush it to disk, copy any
    // existing <path> to <path>.bak (one rolling backup), then atomically move the
    // temp over <path>. On any failure returns Err and leaves <path> untouched.
    static Result<void, Error> writeFileAtomic(const std::filesystem::path& path,
                                               const std::string& content);

    // Read binary file, with the same limit as readFile
    static Result<std::vector<uint8_t>, Error> readBinaryFile(const std::filesystem::path& path,
                                                              size_t maxBytes = SIZE_MAX);

    static bool exists(const std::filesystem::path& path);

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
