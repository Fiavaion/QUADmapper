#include "ErrorNotification.hpp"
#include "Logger.hpp"
#include <algorithm>
#include <sstream>

namespace quadmapper {

// ErrorNotification implementation
ErrorNotification::ErrorNotification(const std::string& title, const std::string& message, ErrorSeverity severity)
    : title(title)
    , message(message)
    , severity(severity)
    , timestamp(std::chrono::steady_clock::now())
{
}

// ErrorNotificationManager implementation
ErrorNotificationManager::ErrorNotificationManager() = default;

ErrorNotificationManager& ErrorNotificationManager::getInstance() {
    static ErrorNotificationManager instance;
    return instance;
}

void ErrorNotificationManager::showInfo(const std::string& title, const std::string& message, float duration) {
    ErrorNotification notification(title, message, ErrorSeverity::Info);
    notification.displayDuration = duration;
    show(notification);
}

void ErrorNotificationManager::showWarning(const std::string& title, const std::string& message, float duration) {
    ErrorNotification notification(title, message, ErrorSeverity::Warning);
    notification.displayDuration = duration;
    show(notification);
}

void ErrorNotificationManager::showError(const std::string& title, const std::string& message, float duration) {
    ErrorNotification notification(title, message, ErrorSeverity::Error);
    notification.displayDuration = duration;
    show(notification);
}

void ErrorNotificationManager::showCritical(const std::string& title, const std::string& message) {
    ErrorNotification notification(title, message, ErrorSeverity::Critical);
    notification.displayDuration = 0.0f;  // Persistent
    show(notification);
}

void ErrorNotificationManager::show(const ErrorNotification& notification) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Log the notification
    switch (notification.severity) {
        case ErrorSeverity::Info:
            LOG_INFO("[{}] {}", notification.title, notification.message);
            break;
        case ErrorSeverity::Warning:
            LOG_WARNING("[{}] {}", notification.title, notification.message);
            break;
        case ErrorSeverity::Error:
        case ErrorSeverity::Critical:
            LOG_ERROR("[{}] {}", notification.title, notification.message);
            break;
    }

    // Add to active notifications
    activeNotifications_.push_front(notification);

    // Limit active notifications
    while (activeNotifications_.size() > MAX_NOTIFICATIONS) {
        activeNotifications_.pop_back();
    }

    // Add to history
    addToHistory(notification);

    // Fire callback
    if (notificationCallback_) {
        notificationCallback_(notification);
    }
}

void ErrorNotificationManager::dismiss(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (index < activeNotifications_.size()) {
        activeNotifications_.erase(activeNotifications_.begin() + index);
    }
}

void ErrorNotificationManager::dismissAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    activeNotifications_.clear();
}

void ErrorNotificationManager::update(float deltaTime) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();

    // Remove expired notifications (those with duration > 0)
    activeNotifications_.erase(
        std::remove_if(activeNotifications_.begin(), activeNotifications_.end(),
            [now](const ErrorNotification& n) {
                if (n.displayDuration <= 0.0f) {
                    return false;  // Persistent notification
                }
                auto elapsed = std::chrono::duration<float>(now - n.timestamp).count();
                return elapsed >= n.displayDuration;
            }),
        activeNotifications_.end()
    );
}

size_t ErrorNotificationManager::countBySeverity(ErrorSeverity severity) const {
    std::lock_guard<std::mutex> lock(mutex_);

    return std::count_if(activeNotifications_.begin(), activeNotifications_.end(),
        [severity](const ErrorNotification& n) {
            return n.severity == severity;
        });
}

void ErrorNotificationManager::addToHistory(const ErrorNotification& notification) {
    history_.push_front(notification);

    while (history_.size() > MAX_HISTORY) {
        history_.pop_back();
    }
}

void ErrorNotificationManager::clearHistory() {
    std::lock_guard<std::mutex> lock(mutex_);
    history_.clear();
}

// ErrorMessages implementation
namespace ErrorMessages {

std::string fileNotFound(const std::string& path) {
    return "The file '" + path + "' could not be found. Please check that the file exists and the path is correct.";
}

std::string fileReadError(const std::string& path) {
    return "Failed to read file '" + path + "'. The file may be corrupted, in use by another application, or you may not have permission to access it.";
}

std::string fileWriteError(const std::string& path) {
    return "Failed to write to '" + path + "'. Please check that you have write permission and sufficient disk space.";
}

std::string invalidFileFormat(const std::string& path, const std::string& expectedFormat) {
    return "The file '" + path + "' is not a valid " + expectedFormat + " file or may be corrupted.";
}

std::string mediaLoadFailed(const std::string& path) {
    return "Failed to load media file '" + path + "'. The file format may not be supported or the file may be corrupted.";
}

std::string videoDecodeFailed(const std::string& path, const std::string& details) {
    std::ostringstream oss;
    oss << "Video decoding failed for '" << path << "'.";
    if (!details.empty()) {
        oss << " Details: " << details;
    }
    return oss.str();
}

std::string audioInitFailed(const std::string& details) {
    std::ostringstream oss;
    oss << "Audio system initialization failed.";
    if (!details.empty()) {
        oss << " Details: " << details;
    }
    oss << " Audio playback may not be available.";
    return oss.str();
}

std::string projectLoadFailed(const std::string& path) {
    return "Failed to load project from '" + path + "'. The project file may be corrupted or from an incompatible version.";
}

std::string projectSaveFailed(const std::string& path) {
    return "Failed to save project to '" + path + "'. Please check disk space and write permissions.";
}

std::string projectVersionMismatch(const std::string& fileVersion, const std::string& appVersion) {
    return "Project version (" + fileVersion + ") differs from application version (" + appVersion + "). Some features may not work correctly.";
}

std::string openGLError(unsigned int errorCode) {
    std::ostringstream oss;
    oss << "OpenGL error occurred (code: 0x" << std::hex << errorCode << "). ";

    switch (errorCode) {
        case 0x0500: oss << "Invalid enum"; break;
        case 0x0501: oss << "Invalid value"; break;
        case 0x0502: oss << "Invalid operation"; break;
        case 0x0503: oss << "Stack overflow"; break;
        case 0x0504: oss << "Stack underflow"; break;
        case 0x0505: oss << "Out of memory"; break;
        case 0x0506: oss << "Invalid framebuffer operation"; break;
        default: oss << "Unknown error"; break;
    }

    return oss.str();
}

std::string shaderCompileFailed(const std::string& shaderName, const std::string& log) {
    std::ostringstream oss;
    oss << "Shader '" << shaderName << "' failed to compile.";
    if (!log.empty()) {
        oss << "\n" << log;
    }
    return oss.str();
}

std::string shaderLinkFailed(const std::string& programName, const std::string& log) {
    std::ostringstream oss;
    oss << "Shader program '" << programName << "' failed to link.";
    if (!log.empty()) {
        oss << "\n" << log;
    }
    return oss.str();
}

std::string textureCreationFailed(const std::string& details) {
    std::ostringstream oss;
    oss << "Failed to create texture.";
    if (!details.empty()) {
        oss << " " << details;
    }
    return oss.str();
}

std::string outOfMemory() {
    return "The application has run out of memory. Try closing other applications or reducing the number of media items.";
}

std::string unexpectedError(const std::string& context) {
    return "An unexpected error occurred" + (context.empty() ? "." : " in " + context + ".") +
           " Please save your work and restart the application if issues persist.";
}

} // namespace ErrorMessages

} // namespace quadmapper
