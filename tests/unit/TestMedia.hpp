#pragma once

// Real media for the tests that need it. It lives outside the repo: in the folder the
// QM_TEST_MEDIA environment variable names, else D:/testing Videos (the development
// PC). A test skips when its clip is absent.

#include <cstdlib>
#include <filesystem>
#include <string>

inline std::filesystem::path testMediaDir() {
    const char* folder = std::getenv("QM_TEST_MEDIA");
    return std::filesystem::path(folder && *folder ? folder : "D:/testing Videos");
}

inline std::filesystem::path testMediaFile(const std::string& relative) {
    return testMediaDir() / relative;
}
