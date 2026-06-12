#pragma once

#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <deque>
#include <mutex>

namespace quadmapper {

// Error severity levels for notifications
enum class ErrorSeverity {
    Info,       // Informational message
    Warning,    // Warning - operation completed with issues
    Error,      // Error - operation failed but app continues
    Critical    // Critical - major failure, app may be unstable
};

// A single error notification
struct ErrorNotification {
    std::string title;
    std::string message;
    ErrorSeverity severity = ErrorSeverity::Error;
    std::chrono::steady_clock::time_point timestamp;
    float displayDuration = 5.0f;  // Seconds to display (0 = persistent until dismissed)
    bool dismissed = false;

    ErrorNotification() = default;
    ErrorNotification(const std::string& title, const std::string& message, ErrorSeverity severity);
};

// ErrorNotificationManager manages error display and logging
class ErrorNotificationManager {
public:
    // Default display duration for auto-dismissing notifications
    static constexpr float DEFAULT_DURATION = 5.0f;
    static constexpr size_t MAX_NOTIFICATIONS = 10;
    static constexpr size_t MAX_HISTORY = 100;

    ErrorNotificationManager();
    ~ErrorNotificationManager() = default;

    // Non-copyable
    ErrorNotificationManager(const ErrorNotificationManager&) = delete;
    ErrorNotificationManager& operator=(const ErrorNotificationManager&) = delete;

    // Get singleton instance
    static ErrorNotificationManager& getInstance();

    // Show a notification
    void showInfo(const std::string& title, const std::string& message, float duration = DEFAULT_DURATION);
    void showWarning(const std::string& title, const std::string& message, float duration = DEFAULT_DURATION);
    void showError(const std::string& title, const std::string& message, float duration = DEFAULT_DURATION);
    void showCritical(const std::string& title, const std::string& message);  // Critical is persistent

    // Show notification with custom settings
    void show(const ErrorNotification& notification);

    // Dismiss a notification
    void dismiss(size_t index);
    void dismissAll();

    // Update - remove expired notifications
    void update(float deltaTime);

    // Get active notifications for display
    const std::deque<ErrorNotification>& getActiveNotifications() const { return activeNotifications_; }

    // Get notification history
    const std::deque<ErrorNotification>& getHistory() const { return history_; }

    // Check if there are any active notifications
    bool hasActiveNotifications() const { return !activeNotifications_.empty(); }

    // Count by severity
    size_t countBySeverity(ErrorSeverity severity) const;

    // Callback when a new notification is added
    using NotificationCallback = std::function<void(const ErrorNotification&)>;
    void setNotificationCallback(NotificationCallback callback) {
        notificationCallback_ = std::move(callback);
    }

    // Clear history
    void clearHistory();

private:
    void addToHistory(const ErrorNotification& notification);

    std::deque<ErrorNotification> activeNotifications_;
    std::deque<ErrorNotification> history_;
    NotificationCallback notificationCallback_;
    mutable std::mutex mutex_;
};

// Common error messages and recovery helpers
namespace ErrorMessages {
    // File errors
    std::string fileNotFound(const std::string& path);
    std::string fileReadError(const std::string& path);
    std::string fileWriteError(const std::string& path);
    std::string invalidFileFormat(const std::string& path, const std::string& expectedFormat);

    // Media errors
    std::string mediaLoadFailed(const std::string& path);
    std::string videoDecodeFailed(const std::string& path, const std::string& details);
    std::string audioInitFailed(const std::string& details);

    // Project errors
    std::string projectLoadFailed(const std::string& path);
    std::string projectSaveFailed(const std::string& path);
    std::string projectVersionMismatch(const std::string& fileVersion, const std::string& appVersion);

    // OpenGL errors
    std::string openGLError(unsigned int errorCode);
    std::string shaderCompileFailed(const std::string& shaderName, const std::string& log);
    std::string shaderLinkFailed(const std::string& programName, const std::string& log);
    std::string textureCreationFailed(const std::string& details);

    // General
    std::string outOfMemory();
    std::string unexpectedError(const std::string& context);
}

// Convenience macro for error handling with notification
#define NOTIFY_ERROR(title, message) \
    quadmapper::ErrorNotificationManager::getInstance().showError(title, message)

#define NOTIFY_WARNING(title, message) \
    quadmapper::ErrorNotificationManager::getInstance().showWarning(title, message)

#define NOTIFY_INFO(title, message) \
    quadmapper::ErrorNotificationManager::getInstance().showInfo(title, message)

} // namespace quadmapper
