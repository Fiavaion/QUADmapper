#pragma once

// The application's preferences and their file (ADR-051 D2). The Edit > Preferences
// window that edits them is ui/dialogs/PreferencesDialog.

#include "core/Result.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace quadmapper {

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

    // Crossfade of a scene take from the 1-9 keys, MIDI and a one-argument OSC take
    // (ADR-047 §5), in seconds, 0 to MAX_SCENE_FADE_SECONDS; 0 = a cut.
    float sceneFadeSeconds = 0.0f;
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

    /**
     * Load from a file. Values of the wrong type or out of range are skipped (the
     * current value stays), as are keys this version no longer has.
     */
    Result<void, Error> loadFromFile(const std::filesystem::path& path);

    /** Save to a file, atomically. */
    Result<void, Error> saveToFile(const std::filesystem::path& path) const;
};

}  // namespace quadmapper
