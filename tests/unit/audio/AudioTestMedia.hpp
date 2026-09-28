#pragma once

// Real media for the audio tests. It lives outside the repo (D:\testing Videos, see
// the project notes); tests that need it skip when it is absent.

#include <filesystem>
#include <string>

namespace quadmapper::audio_test {

inline std::filesystem::path mediaPath(const std::string& relative) {
    const std::filesystem::path path = std::filesystem::path("D:/testing Videos") / relative;
    std::error_code ec;
    return std::filesystem::exists(path, ec) ? path : std::filesystem::path();
}

}  // namespace quadmapper::audio_test
