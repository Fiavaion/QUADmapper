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
    static std::string getLogFilePath();

    // Console output control
    static void setConsoleOutput(bool enabled);

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

    // One pass over the format: text an argument inserts is never searched for the next
    // {}, so a file name that contains "{}" cannot shift the arguments after it.
    template<typename T, typename... Rest>
    static std::string formatMessage(const std::string& format, T&& first, Rest&&... rest) {
        const std::string values[] = {formatArg(first), formatArg(rest)...};
        std::string result;
        size_t from = 0;
        for (const std::string& value : values) {
            const size_t pos = format.find("{}", from);
            if (pos == std::string::npos) break;
            result.append(format, from, pos - from).append(value);
            from = pos + 2;
        }
        return result.append(format, from, std::string::npos);
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
