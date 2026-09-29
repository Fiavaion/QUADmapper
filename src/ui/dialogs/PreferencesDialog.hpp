#pragma once

#include "core/Result.hpp"
#include "editor/Preferences.hpp"
#include "ui/AsyncFileDialog.hpp"
#include "ui/panels/ControlPanel.hpp"
#include "ui/panels/Panel.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace quadmapper {

struct EditorContext;
class AudioEngine;
namespace control { class ExternalControl; }

/**
 * The application's preferences (the one copy the app reads) and the Edit >
 * Preferences window that edits them.
 *
 * Every change in the window is applied at once and saved (a slider once, on
 * release, not on every frame of the drag); the media-library
 * settings are read at startup, which the window says. The Control tab edits the
 * same ControlPreferences as the Control window, and the Audio tab switches the same
 * engine as the menu bar's device list, so all of them always agree.
 */
class PreferencesDialog : public Panel {
public:
    static constexpr const char* ID = "Preferences";

    PreferencesDialog() { visible = false; }

    // A Window panel opened from Edit > Preferences... and Ctrl+, (not in the Window
    // menu). ctx.control may be null (no external control in the stress and bench
    // harnesses), as may ctx.audio.
    const char* id() const override { return ID; }
    const char* menuLabel() const override { return nullptr; }
    int windowFlags() const override;
    void beforeBegin(EditorContext& ctx) override;
    // A folder chosen in Browse lands even if the window was closed meanwhile.
    void tick(EditorContext& ctx) override;
    void drawContent(EditorContext& ctx) override;

    const AppPreferences& getPreferences() const { return preferences_; }
    AppPreferences& getPreferences() { return preferences_; }

    /** Load from / save to the config file (the default one unless setConfigPath). */
    Result<void, Error> loadPreferences();
    Result<void, Error> savePreferences();

    /** Use another config file than AppPreferences::getDefaultConfigPath() (tests). */
    void setConfigPath(std::filesystem::path path) { configPath_ = std::move(path); }

private:
    // Each returns true when a preference changed (then saved).
    bool renderGeneralTab(EditorContext& ctx);
    bool renderAudioTab(AudioEngine* audio);
    bool renderControlTab(control::ExternalControl* control);
    void save();
    std::filesystem::path configPath() const;

    AppPreferences preferences_;
    std::filesystem::path configPath_;          // empty = the default config file

    AsyncFileDialog folderDialog_;              // media folder Browse, off the render thread
    ControlSettingsEditor controlSettings_;     // OSC / MIDI settings, as in the Control window
    std::vector<std::string> audioDevices_;     // enumerated when the Audio tab opens
    bool audioTabOpen_ = false;
    std::string saveError_;                     // shown while the last save failed
};

} // namespace quadmapper
