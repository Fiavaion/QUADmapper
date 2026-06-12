#include "Logger.hpp"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace quadmapper {

// Static member initialization
LogLevel Logger::currentLevel_ = LogLevel::Info;
std::unique_ptr<std::ofstream> Logger::logFile_;
std::string Logger::logFilePath_;
bool Logger::consoleEnabled_ = true;
std::mutex Logger::mutex_;

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentLevel_ = level;
}

LogLevel Logger::getLevel() {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentLevel_;
}

bool Logger::enableFileOutput(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Close existing file if open
    if (logFile_) {
        logFile_->close();
        logFile_.reset();
    }

    logFile_ = std::make_unique<std::ofstream>(filePath, std::ios::out | std::ios::app);
    if (!logFile_->is_open()) {
        logFile_.reset();
        logFilePath_.clear();
        return false;
    }

    logFilePath_ = filePath;

    // Write session header
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    *logFile_ << "\n========================================\n";
    *logFile_ << "QUADmapper Log Session Started: ";
    *logFile_ << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    *logFile_ << "\n========================================\n\n";
    logFile_->flush();

    return true;
}

void Logger::disableFileOutput() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (logFile_) {
        *logFile_ << "\n========================================\n";
        *logFile_ << "Log Session Ended\n";
        *logFile_ << "========================================\n";
        logFile_->close();
        logFile_.reset();
    }
    logFilePath_.clear();
}

bool Logger::isFileOutputEnabled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return logFile_ && logFile_->is_open();
}

std::string Logger::getLogFilePath() {
    std::lock_guard<std::mutex> lock(mutex_);
    return logFilePath_;
}

void Logger::setConsoleOutput(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    consoleEnabled_ = enabled;
}

bool Logger::isConsoleOutputEnabled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return consoleEnabled_;
}

void Logger::trace(const std::string& message) {
    if (currentLevel_ <= LogLevel::Trace) {
        log(LogLevel::Trace, message);
    }
}

void Logger::debug(const std::string& message) {
    if (currentLevel_ <= LogLevel::Debug) {
        log(LogLevel::Debug, message);
    }
}

void Logger::info(const std::string& message) {
    if (currentLevel_ <= LogLevel::Info) {
        log(LogLevel::Info, message);
    }
}

void Logger::warning(const std::string& message) {
    if (currentLevel_ <= LogLevel::Warning) {
        log(LogLevel::Warning, message);
    }
}

void Logger::error(const std::string& message) {
    if (currentLevel_ <= LogLevel::Error) {
        log(LogLevel::Error, message);
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Get timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << " ";

    // Level prefix
    switch (level) {
        case LogLevel::Trace:   oss << "[TRACE] "; break;
        case LogLevel::Debug:   oss << "[DEBUG] "; break;
        case LogLevel::Info:    oss << "[INFO]  "; break;
        case LogLevel::Warning: oss << "[WARN]  "; break;
        case LogLevel::Error:   oss << "[ERROR] "; break;
    }

    oss << message;
    std::string formattedMessage = oss.str();

    // Output to console
    if (consoleEnabled_) {
        if (level >= LogLevel::Warning) {
            std::cerr << formattedMessage << std::endl;
        } else {
            std::cout << formattedMessage << std::endl;
        }
    }

    // Output to file
    if (logFile_ && logFile_->is_open()) {
        // Include date in file output
        std::ostringstream dateOss;
        dateOss << std::put_time(&tm, "%Y-%m-%d ");
        *logFile_ << dateOss.str() << formattedMessage << std::endl;

        // Flush on errors immediately
        if (level >= LogLevel::Error) {
            logFile_->flush();
        }
    }
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (logFile_ && logFile_->is_open()) {
        logFile_->flush();
    }
    std::cout.flush();
    std::cerr.flush();
}

} // namespace quadmapper
