#include <gtest/gtest.h>
#include "core/Logger.hpp"

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

} // namespace test
} // namespace quadmapper
