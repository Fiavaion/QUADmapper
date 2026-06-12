#include "FileSystem.hpp"
#include <fstream>
#include <algorithm>

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
