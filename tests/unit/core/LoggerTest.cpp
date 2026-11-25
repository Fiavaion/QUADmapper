#include <gtest/gtest.h>
#include "core/Logger.hpp"
#include "core/FileSystem.hpp"
#include "core/UUID.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace quadmapper {
namespace test {

TEST(LoggerTest, SetLevelChangesCurrentLevel) {
    Logger::setLevel(LogLevel::Warning);
    EXPECT_EQ(Logger::getLevel(), LogLevel::Warning);

    Logger::setLevel(LogLevel::Debug);
    EXPECT_EQ(Logger::getLevel(), LogLevel::Debug);
}

TEST(LoggerTest, LogLevelOrdering) {
    // Verify log levels are ordered correctly
    EXPECT_LT(static_cast<int>(LogLevel::Trace), static_cast<int>(LogLevel::Debug));
    EXPECT_LT(static_cast<int>(LogLevel::Debug), static_cast<int>(LogLevel::Info));
    EXPECT_LT(static_cast<int>(LogLevel::Info), static_cast<int>(LogLevel::Warning));
    EXPECT_LT(static_cast<int>(LogLevel::Warning), static_cast<int>(LogLevel::Error));
}

TEST(LoggerTest, LoggingDoesNotCrash) {
    // Verify logging calls don't crash
    Logger::setLevel(LogLevel::Trace);

    EXPECT_NO_THROW(Logger::trace("trace message"));
    EXPECT_NO_THROW(Logger::debug("debug message"));
    EXPECT_NO_THROW(Logger::info("info message"));
    EXPECT_NO_THROW(Logger::warning("warning message"));
    EXPECT_NO_THROW(Logger::error("error message"));
}

TEST(LoggerTest, FormatStringWorks) {
    // Test that format strings don't crash
    EXPECT_NO_THROW(Logger::info("Value: {}", 42));
    EXPECT_NO_THROW(Logger::info("Values: {} and {}", 1, 2));
    EXPECT_NO_THROW(Logger::info("String: {}", "test"));
}

// An argument's own text is not searched for the next {}: a name containing "{}" must
// not take the value meant for the placeholder after it.
TEST(LoggerTest, AnArgumentContainingBracesDoesNotShiftTheNextOne) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("qm_log_" + UUID::generate().toString());
    fs::create_directories(dir);
    const fs::path file = dir / "format.log";

    ASSERT_TRUE(Logger::enableFileOutput(utf8FromPath(file)));
    Logger::error("file {} has {} frames{}", "clip{}.mov", 7);
    Logger::disableFileOutput();

    std::ifstream in(file);
    std::ostringstream text;
    text << in.rdbuf();
    EXPECT_NE(text.str().find("file clip{}.mov has 7 frames{}"), std::string::npos) << text.str();

    in.close();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// The log path is UTF-8: a user folder with a non-ASCII name must still get a log file.
TEST(LoggerTest, FileOutputOpensInsideNonAsciiFolder) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("qm_log_" + UUID::generate().toString()) /
                         pathFromUtf8("Jos\xC3\xA9" " \xD0\x9F");
    fs::create_directories(dir);
    const fs::path file = dir / "quadmapper.log";

    ASSERT_TRUE(Logger::enableFileOutput(utf8FromPath(file)));
    Logger::error("written to the file");
    Logger::disableFileOutput();

    std::ifstream in(file);
    std::ostringstream text;
    text << in.rdbuf();
    EXPECT_NE(text.str().find("written to the file"), std::string::npos);

    std::error_code ec;
    fs::remove_all(dir.parent_path(), ec);
}

} // namespace test
} // namespace quadmapper
