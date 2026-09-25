#include "FileSystem.hpp"
#include <fstream>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace quadmapper {

Result<std::string, Error> FileSystem::readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::in);
    if (!file) {
        return Err<std::string>("Failed to open file: " + path.string());
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    return Ok(std::move(content));
}

Result<void, Error> FileSystem::writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file) {
        return Err("Failed to open file for writing: " + path.string());
    }

    file << content;
    if (!file) {
        return Err("Failed to write to file: " + path.string());
    }

    return Ok();
}

Result<void, Error> FileSystem::writeFileAtomic(const std::filesystem::path& path,
                                                const std::string& content) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    std::error_code ec;

#ifdef _WIN32
    // Win32 so the temp is truly on disk (FlushFileBuffers) before it replaces the
    // real file; an ofstream flush only reaches the OS cache.
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return Err("Failed to open file for writing: " + tmp.string() +
                   " (error " + std::to_string(GetLastError()) + ")");
    }
    DWORD written = 0;
    const bool wrote = WriteFile(h, content.data(), static_cast<DWORD>(content.size()),
                                 &written, nullptr) &&
                       written == content.size();
    const bool flushed = wrote && FlushFileBuffers(h);
    const bool closed = CloseHandle(h) != 0;
    if (!wrote || !flushed || !closed) {
        std::filesystem::remove(tmp, ec);
        return Err("Failed to write file: " + tmp.string());
    }
#else
    {
        std::ofstream file(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!file) {
            return Err("Failed to open file for writing: " + tmp.string());
        }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        file.close();
        if (file.fail()) {
            std::filesystem::remove(tmp, ec);
            return Err("Failed to write file: " + tmp.string());
        }
    }
#endif

    if (std::filesystem::exists(path, ec)) {
        std::filesystem::path bak = path;
        bak += ".bak";
        std::filesystem::copy_file(path, bak, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return Err("Failed to back up " + path.string() + " before saving");
        }
    }

#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = GetLastError();
        std::filesystem::remove(tmp, ec);
        return Err("Failed to replace " + path.string() + " (error " + std::to_string(err) + ")");
    }
#else
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return Err("Failed to replace " + path.string() + ": " + ec.message());
    }
#endif
    return Ok();
}

Result<std::vector<uint8_t>, Error> FileSystem::readBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) {
        return Err<std::vector<uint8_t>>("Failed to open file: " + path.string());
    }

    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);

    return Ok(std::move(data));
}

Result<void, Error> FileSystem::writeBinaryFile(const std::filesystem::path& path,
                                                const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file) {
        return Err("Failed to open file for writing: " + path.string());
    }

    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    if (!file) {
        return Err("Failed to write to file: " + path.string());
    }

    return Ok();
}

bool FileSystem::exists(const std::filesystem::path& path) {
    return std::filesystem::exists(path);
}

bool FileSystem::isDirectory(const std::filesystem::path& path) {
    return std::filesystem::is_directory(path);
}

Result<void, Error> FileSystem::createDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return Err("Failed to create directory: " + ec.message());
    }
    return Ok();
}

std::string FileSystem::getExtension(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

std::string FileSystem::getStem(const std::filesystem::path& path) {
    return path.stem().string();
}

std::filesystem::path FileSystem::makeRelative(const std::filesystem::path& path,
                                               const std::filesystem::path& base) {
    return std::filesystem::relative(path, base);
}

std::filesystem::path FileSystem::resolveRelative(const std::filesystem::path& relativePath,
                                                  const std::filesystem::path& base) {
    return std::filesystem::absolute(base / relativePath);
}

} // namespace quadmapper
