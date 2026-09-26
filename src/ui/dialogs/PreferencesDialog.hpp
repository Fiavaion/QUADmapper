#pragma once

#include "core/Result.hpp"
#include "input/KeyboardShortcuts.hpp"
#include <string>
#include <vector>
#include <memory>

namespace quadmapper {

/**
 * General application preferences.
 */
struct GeneralPreferences {
    // Project defaults
    std::string defaultProjectPath;
    int defaultMeshResolution = 8;
    bool openLastProjectOnStartup = true;
    int recentProjectsMaxCount = 10;

    // Media Library settings
    std::string defaultMediaFolder;           // Default folder to scan for media
    bool autoLoadMediaOnStartup = true;       // Auto-load media from default folder
    bool recursiveFolderScan = false;         // Scan subdirectories
    std::vector<std::string> mediaFolderBookmarks;  // Saved folder paths for quick access

    // Auto-save settings
    bool autoSaveEnabled = true;
    int autoSaveIntervalMinutes = 1;          // ADR-046: a crash costs at most ~1 minute
    bool createBackupOnSave = true;

    // UI preferences
    bool showWelcomeDialog = true;
    bool confirmOnExit = true;
    bool rememberWindowLayout = true;

    // Theme
    int themeIndex = 0;  // 0 = Dark, 1 = Light

    // UI complexity mode (ADR-042): 0 = Simple, 1 = Advanced. Default Simple.
    int uiMode = 0;
};

/**
 * Performance-related preferences.
 */
struct PerformancePreferences {
    // Video decoding
    int videoBufferFrames = 30;
    int videoDecodeThreads = 2;
    bool enableHardwareDecoding = true;

    // Rendering
    bool enableVSync = true;
    int targetFrameRate = 60;
    int textureQuality = 2;  // 0 = Low, 1 = Medium, 2 = High
    bool enableMipmaps = true;

    // Audio
    int audioBufferSize = 512;  // samples
    int audioSampleRate = 48000;
    std::string audioDevice;    // output device name; empty = the system default (ADR-050)

    // Output
    int outputBufferCount = 2;  // double or triple buffering
    bool enableAdaptiveSync = false;
};

/**
 * External control (ADR-048): which MIDI inputs to open and how OSC listens. Machine
 * setup, not show content, so it lives here rather than in the project.
 */
struct ControlPreferences {
    bool oscEnabled = false;                 // off by default: enabling opens a UDP port
    int oscPort = 8000;
    std::string oscBindAddress = "0.0.0.0";  // every interface
    std::vector<std::string> midiDevices;    // input devices opened by name when present
    // OSC feedback: current values sent back to the controller (off by default).
    bool oscFeedbackEnabled = false;
    std::string oscFeedbackHost;             // dotted IPv4; empty = reply to the sender
    int oscFeedbackPort = 9000;              // 0 = the sender's own port (reply mode only)
    bool oscFeedbackUseIds = false;          // address entities by UUID, not unique name
};

/**
 * All application preferences combined.
 */
struct AppPreferences {
    GeneralPreferences general;
    PerformancePreferences performance;
    ControlPreferences control;
    // Keyboard shortcuts are managed separately

    static std::string getDefaultConfigPath();
};

/**
 * Preferences dialog for QUADmapper.
 * Provides tabs for General, Keyboard Shortcuts, and Performance settings.
 */
class PreferencesDialog {
public:
    PreferencesDialog();
    ~PreferencesDialog() = default;

    /**
     * Show the preferences dialog.
     */
    void show();

    /**
     * Hide the preferences dialog.
     */
    void hide();

    /**
     * Check if dialog is visible.
     */
    bool isVisible() const { return visible_; }

    /**
     * Render the dialog. Call each frame.
     */
    void render();

    /**
     * Set the keyboard shortcuts manager to use for shortcut editing.
     */
    void setKeyboardShortcuts(KeyboardShortcuts* shortcuts);

    /**
     * Get current preferences.
     */
    const AppPreferences& getPreferences() const { return preferences_; }

    /**
     * Get mutable preferences.
     */
    AppPreferences& getPreferences() { return preferences_; }

    /**
     * Load preferences from the default config file.
     */
    Result<void, Error> loadPreferences();

    /**
     * Save preferences to the default config file.
     */
    Result<void, Error> savePreferences();

    /**
     * Load preferences from a specific file.
     */
    Result<void, Error> loadFromFile(const std::string& path);

    /**
     * Save preferences to a specific file.
     */
    Result<void, Error> saveToFile(const std::string& path) const;

    /**
     * Reset all preferences to defaults.
     */
    void resetToDefaults();

    /**
     * Check if preferences have been modified since last save.
     */
    bool isModified() const { return modified_; }

private:
    void renderGeneralTab();
    void renderKeyboardShortcutsTab();
    void renderPerformanceTab();

    // Shortcut editing helpers
    void renderShortcutEntry(ShortcutInfo& info);
    void startShortcutCapture(ShortcutAction action);
    void stopShortcutCapture();
    void handleShortcutCapture();

    bool visible_ = false;
    bool modified_ = false;
    int currentTab_ = 0;

    AppPreferences preferences_;
    AppPreferences savedPreferences_;  // For detecting changes

    KeyboardShortcuts* shortcuts_ = nullptr;

    // Shortcut capture state
    bool capturingShortcut_ = false;
    ShortcutAction captureAction_ = ShortcutAction::COUNT;
    std::string captureCategory_;
    float captureTimeRemaining_ = 0.0f;

    // Category filter for shortcuts tab
    std::string shortcutFilterCategory_;
    std::string shortcutSearchFilter_;
};

} // namespace quadmapper
