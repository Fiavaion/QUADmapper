#pragma once

// Real media for the audio tests (TestMedia.hpp says where it lives); tests that need
// it skip when it is absent.

#include "../TestMedia.hpp"

#include <filesystem>
#include <string>

namespace quadmapper::audio_test {

inline std::filesystem::path mediaPath(const std::string& relative) {
    const std::filesystem::path path = testMediaFile(relative);
    std::error_code ec;
    return std::filesystem::exists(path, ec) ? path : std::filesystem::path();
}

}  // namespace quadmapper::audio_test
