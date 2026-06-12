#include <gtest/gtest.h>
#include "core/ErrorNotification.hpp"
#include <thread>
#include <chrono>

namespace quadmapper {
namespace test {

class ErrorNotificationManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = std::make_unique<ErrorNotificationManager>();
    }

    std::unique_ptr<ErrorNotificationManager> manager_;
};

TEST_F(ErrorNotificationManagerTest, InitialStateHasNoNotifications) {
    EXPECT_FALSE(manager_->hasActiveNotifications());
    EXPECT_TRUE(manager_->getActiveNotifications().empty());
}

TEST_F(ErrorNotificationManagerTest, ShowInfoAddsNotification) {
    manager_->showInfo("Title", "Message");

    EXPECT_TRUE(manager_->hasActiveNotifications());
    EXPECT_EQ(manager_->getActiveNotifications().size(), 1);

    const auto& notification = manager_->getActiveNotifications().front();
    EXPECT_EQ(notification.title, "Title");
    EXPECT_EQ(notification.message, "Message");
    EXPECT_EQ(notification.severity, ErrorSeverity::Info);
}

TEST_F(ErrorNotificationManagerTest, ShowWarningAddsWarning) {
    manager_->showWarning("Warn", "Message");

    const auto& notification = manager_->getActiveNotifications().front();
    EXPECT_EQ(notification.severity, ErrorSeverity::Warning);
}

TEST_F(ErrorNotificationManagerTest, ShowErrorAddsError) {
    manager_->showError("Error", "Message");

    const auto& notification = manager_->getActiveNotifications().front();
    EXPECT_EQ(notification.severity, ErrorSeverity::Error);
}

TEST_F(ErrorNotificationManagerTest, ShowCriticalAddsCritical) {
    manager_->showCritical("Critical", "Message");

    const auto& notification = manager_->getActiveNotifications().front();
    EXPECT_EQ(notification.severity, ErrorSeverity::Critical);
    EXPECT_FLOAT_EQ(notification.displayDuration, 0.0f);  // Persistent
}

TEST_F(ErrorNotificationManagerTest, DismissRemovesNotification) {
    manager_->showInfo("One", "First");
    manager_->showInfo("Two", "Second");

    EXPECT_EQ(manager_->getActiveNotifications().size(), 2);

    manager_->dismiss(0);

    EXPECT_EQ(manager_->getActiveNotifications().size(), 1);
}

TEST_F(ErrorNotificationManagerTest, DismissAllClearsNotifications) {
    manager_->showInfo("One", "First");
    manager_->showInfo("Two", "Second");
    manager_->showInfo("Three", "Third");

    manager_->dismissAll();

    EXPECT_FALSE(manager_->hasActiveNotifications());
}

TEST_F(ErrorNotificationManagerTest, CountBySeverity) {
    manager_->showInfo("Info1", "Message");
    manager_->showInfo("Info2", "Message");
    manager_->showWarning("Warn", "Message");
    manager_->showError("Error", "Message");

    EXPECT_EQ(manager_->countBySeverity(ErrorSeverity::Info), 2);
    EXPECT_EQ(manager_->countBySeverity(ErrorSeverity::Warning), 1);
    EXPECT_EQ(manager_->countBySeverity(ErrorSeverity::Error), 1);
    EXPECT_EQ(manager_->countBySeverity(ErrorSeverity::Critical), 0);
}

TEST_F(ErrorNotificationManagerTest, HistoryTracksAllNotifications) {
    manager_->showInfo("First", "Message");
    manager_->showInfo("Second", "Message");
    manager_->dismissAll();
    manager_->showInfo("Third", "Message");

    EXPECT_EQ(manager_->getHistory().size(), 3);
}

TEST_F(ErrorNotificationManagerTest, ClearHistoryRemovesHistory) {
    manager_->showInfo("Test", "Message");
    manager_->clearHistory();

    EXPECT_TRUE(manager_->getHistory().empty());
}

TEST_F(ErrorNotificationManagerTest, CallbackCalledOnNotification) {
    int callbackCount = 0;
    ErrorNotification lastNotification;

    manager_->setNotificationCallback([&callbackCount, &lastNotification](const ErrorNotification& n) {
        ++callbackCount;
        lastNotification = n;
    });

    manager_->showInfo("Test", "Message");

    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(lastNotification.title, "Test");
}

TEST_F(ErrorNotificationManagerTest, MaxNotificationsLimited) {
    // Add more than MAX_NOTIFICATIONS
    for (size_t i = 0; i < ErrorNotificationManager::MAX_NOTIFICATIONS + 5; ++i) {
        manager_->showInfo("Title", "Message");
    }

    EXPECT_EQ(manager_->getActiveNotifications().size(), ErrorNotificationManager::MAX_NOTIFICATIONS);
}

// Test ErrorMessages namespace
TEST(ErrorMessagesTest, FileNotFoundMessage) {
    std::string msg = ErrorMessages::fileNotFound("/path/to/file.txt");
    EXPECT_NE(msg.find("/path/to/file.txt"), std::string::npos);
    EXPECT_NE(msg.find("not"), std::string::npos);
}

TEST(ErrorMessagesTest, OpenGLErrorMessage) {
    std::string msg = ErrorMessages::openGLError(0x0500);
    EXPECT_NE(msg.find("Invalid enum"), std::string::npos);
}

TEST(ErrorMessagesTest, ShaderCompileFailedMessage) {
    std::string msg = ErrorMessages::shaderCompileFailed("vertex", "error on line 5");
    EXPECT_NE(msg.find("vertex"), std::string::npos);
    EXPECT_NE(msg.find("error on line 5"), std::string::npos);
}

TEST(ErrorMessagesTest, OutOfMemoryMessage) {
    std::string msg = ErrorMessages::outOfMemory();
    EXPECT_NE(msg.find("memory"), std::string::npos);
}

// Test ErrorNotification struct
TEST(ErrorNotificationTest, ConstructorSetsFields) {
    ErrorNotification n("Title", "Message", ErrorSeverity::Warning);

    EXPECT_EQ(n.title, "Title");
    EXPECT_EQ(n.message, "Message");
    EXPECT_EQ(n.severity, ErrorSeverity::Warning);
    EXPECT_FALSE(n.dismissed);
}

TEST(ErrorNotificationTest, DefaultDisplayDuration) {
    ErrorNotification n;
    EXPECT_FLOAT_EQ(n.displayDuration, 5.0f);
}

} // namespace test
} // namespace quadmapper
