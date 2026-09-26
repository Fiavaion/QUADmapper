#pragma once

#include "core/Result.hpp"
#include "ui/AsyncFileDialog.hpp"
#include "ui/panels/ControlPanel.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace quadmapper {

struct EditorContext;
class AudioEngine;
namespace control { class ExternalControl; }

/**
 * General application preferences. Every field is honoured by the app.
 */
struct GeneralPreferences {
    // Media library, read at startup
    std::string defaultMediaFolder;       // scanned at startup and watched for new files
    bool autoLoadMediaOnStartup = true;   // import the folder's media at startup
    bool recursiveFolderScan = false;     // include subdirectories (scan and watch)

    // Auto-save (ADR-046 §4), applied at once
    bool autoSaveEnabled = true;
    int autoSaveIntervalMinutes = 1;      // ADR-046: a crash costs at most ~1 minute

    // UI complexity mode (ADR-042): 0 = Simple, 1 = Advanced. The last mode used.
    int uiMode = 0;
};

// Limits the dialog offers and loading enforces.
constexpr int MIN_AUTOSAVE_MINUTES = 1;
constexpr int MAX_AUTOSAVE_MINUTES = 30;

/**
 * Machine audio setup. Stored under "performance" in the file, the section it has
 * always been saved in.
 */
struct PerformancePreferences {
    std::string audioDevice;    // output device name; empty = the system default (ADR-050)
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

    // <Roaming AppData>/QUADmapper/preferences.json (XDG config elsewhere), built as a
    // wide path so a user name outside the ANSI code page works. Creates the folder.
    static std::filesystem::path getDefaultConfigPath();
};

/**
 * The application's preferences (the one copy the app reads) and the Edit >
 * Preferences window that edits them.
 *
 * Every change in the window is applied at once and saved; the media-library
 * settings are read at startup, which the window says. The Control tab edits the
 * same ControlPreferences as the Control window, and the Audio tab switches the same
 * engine as the menu bar's device list, so all of them always agree.
 */
class PreferencesDialog {
public:
    PreferencesDialog() = default;

    /**
     * Render the window while ctx.es.showPreferences is set. `control` may be null
     * (no external control in the stress and bench harnesses), as may ctx.audio.
     */
    void render(EditorContext& ctx, control::ExternalControl* control);

    const AppPreferences& getPreferences() const { return preferences_; }
    AppPreferences& getPreferences() { return preferences_; }

    /** Load from / save to the default config file. */
    Result<void, Error> loadPreferences();
    Result<void, Error> savePreferences();

    /**
     * Load from a specific file. Values of the wrong type or out of range are skipped
     * (the current value stays), as are keys this version no longer has.
     */
    Result<void, Error> loadFromFile(const std::filesystem::path& path);

    /** Save to a specific file. */
    Result<void, Error> saveToFile(const std::filesystem::path& path) const;

private:
    // Each returns true when a preference changed (then saved).
    bool renderGeneralTab(EditorContext& ctx);
    bool renderAudioTab(AudioEngine* audio);
    bool renderControlTab(control::ExternalControl* control);
    void save();

    AppPreferences preferences_;

    AsyncFileDialog folderDialog_;              // media folder Browse, off the render thread
    ControlSettingsEditor controlSettings_;     // OSC / MIDI settings, as in the Control window
    std::vector<std::string> audioDevices_;     // enumerated when the Audio tab opens
    bool audioTabOpen_ = false;
    std::string saveError_;                     // shown while the last save failed
};

} // namespace quadmapper
