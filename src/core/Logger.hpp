#pragma once

#include <atomic>
#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <memory>
#include <mutex>

namespace quadmapper {

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4
};

class Logger {
public:
    static void setLevel(LogLevel level);
    static LogLevel getLevel();

    // File output. The path is UTF-8, like every path held in a string.
    static bool enableFileOutput(const std::string& filePath);
    static void disableFileOutput();
    static bool isFileOutputEnabled();
    static std::string getLogFilePath();

    // Console output control
    static void setConsoleOutput(bool enabled);
    static bool isConsoleOutputEnabled();

    // Basic logging (no format)
    static void trace(const std::string& message);
    static void debug(const std::string& message);
    static void info(const std::string& message);
    static void warning(const std::string& message);
    static void error(const std::string& message);

    // Format string logging (simplified {} placeholder support)
    template<typename... Args>
    static void trace(const std::string& format, Args&&... args) {
        if (currentLevel_ <= LogLevel::Trace) {
            log(LogLevel::Trace, formatMessage(format, std::forward<Args>(args)...));
        }
    }

    template<typename... Args>
    static void debug(const std::string& format, Args&&... args) {
        if (currentLevel_ <= LogLevel::Debug) {
            log(LogLevel::Debug, formatMessage(format, std::forward<Args>(args)...));
        }
    }

    template<typename... Args>
    static void info(const std::string& format, Args&&... args) {
        if (currentLevel_ <= LogLevel::Info) {
            log(LogLevel::Info, formatMessage(format, std::forward<Args>(args)...));
        }
    }

    template<typename... Args>
    static void warning(const std::string& format, Args&&... args) {
        if (currentLevel_ <= LogLevel::Warning) {
            log(LogLevel::Warning, formatMessage(format, std::forward<Args>(args)...));
        }
    }

    template<typename... Args>
    static void error(const std::string& format, Args&&... args) {
        if (currentLevel_ <= LogLevel::Error) {
            log(LogLevel::Error, formatMessage(format, std::forward<Args>(args)...));
        }
    }

    static void log(LogLevel level, const std::string& message);

    // Flush log file immediately (useful before crash)
    static void flush();

private:
    static std::atomic<LogLevel> currentLevel_;  // read by every thread that logs
    static std::unique_ptr<std::ofstream> logFile_;
    static std::string logFilePath_;
    static bool consoleEnabled_;
    static std::mutex mutex_;

    // Simple format helper - replaces {} with args
    template<typename T>
    static std::string formatArg(const T& arg) {
        std::ostringstream oss;
        oss << arg;
        return oss.str();
    }

    template<typename T, typename... Rest>
    static std::string formatMessage(const std::string& format, T&& first, Rest&&... rest) {
        size_t pos = format.find("{}");
        if (pos == std::string::npos) {
            return format;
        }
        std::string result = format.substr(0, pos) + formatArg(first) + format.substr(pos + 2);
        return formatMessage(result, std::forward<Rest>(rest)...);
    }

    static std::string formatMessage(const std::string& format) {
        return format;
    }
};

// Convenience macros (variadic to support format strings)
#define LOG_TRACE(...) quadmapper::Logger::trace(__VA_ARGS__)
#define LOG_DEBUG(...) quadmapper::Logger::debug(__VA_ARGS__)
#define LOG_INFO(...) quadmapper::Logger::info(__VA_ARGS__)
#define LOG_WARNING(...) quadmapper::Logger::warning(__VA_ARGS__)
#define LOG_ERROR(...) quadmapper::Logger::error(__VA_ARGS__)

} // namespace quadmapper
