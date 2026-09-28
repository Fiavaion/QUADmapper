#include "PreferencesDialog.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "audio/AudioEngine.hpp"
#include "scene/SceneFade.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace quadmapper {

namespace {

const ImVec4 kSaveError(1.0f, 0.35f, 0.3f, 1.0f);

void appliesNote(const char* text) {
    ImGui::TextDisabled("%s", text);
}

}  // namespace

// ---- Window -------------------------------------------------------------------

void PreferencesDialog::render(EditorContext& ctx, control::ExternalControl* control) {
    // A folder chosen in Browse lands even if the window was closed meanwhile.
    std::vector<std::string> chosen;
    if (folderDialog_.takeResult(chosen) && !chosen.empty()) {
        preferences_.general.defaultMediaFolder = chosen.front();
        save();
    }

    bool& open = ctx.es.showPreferences;
    if (!open) {
        audioTabOpen_ = false;
        return;
    }

    // Sized in font units so it fits its hints at any display scale.
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(46.0f * em, 34.0f * em), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Preferences", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }

    bool changed = false;
    bool audioTab = false;
    if (ImGui::BeginTabBar("PreferenceTabs")) {
        if (ImGui::BeginTabItem("General")) {
            changed |= renderGeneralTab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            audioTab = true;
            changed |= renderAudioTab(ctx.audio);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Control")) {
            changed |= renderControlTab(control);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    audioTabOpen_ = audioTab;
    if (changed) save();

    if (!saveError_.empty()) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kSaveError);
        ImGui::TextWrapped("Preferences could not be saved: %s", saveError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

bool PreferencesDialog::renderGeneralTab(EditorContext& ctx) {
    GeneralPreferences& general = preferences_.general;
    bool changed = false;

    ImGui::SeparatorText("Interface");
    // The mode is the editor's own state; Application saves it when it changes.
    int mode = ctx.es.uiMode == EditorState::UiMode::Advanced ? 1 : 0;
    const bool simple = ImGui::RadioButton("Simple", &mode, 0);
    ImGui::SameLine();
    const bool advanced = ImGui::RadioButton("Advanced", &mode, 1);
    if (simple || advanced) {
        ctx.es.uiMode = mode == 1 ? EditorState::UiMode::Advanced : EditorState::UiMode::Simple;
    }
    ImGui::SameLine();
    appliesNote("(applies now; QUADmapper starts in the mode last used)");

    ImGui::SeparatorText("Scene takes");
    ImGui::SetNextItemWidth(200.0f);
    // Applies while dragged; saved once, on release. Ctrl+click types a value.
    ImGui::SliderFloat("Crossfade (seconds)", &general.sceneFadeSeconds, 0.0f, MAX_SCENE_FADE_SECONDS,
                       "%.2f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    appliesNote("0 = a cut. For the 1-9 keys, MIDI and OSC /qm/scene/take n;");
    appliesNote("/qm/scene/take n seconds sets its own.");

    ImGui::SeparatorText("Auto-save");
    changed |= ImGui::Checkbox("Save unsaved work to a recovery file", &general.autoSaveEnabled);
    ImGui::BeginDisabled(!general.autoSaveEnabled);
    ImGui::SetNextItemWidth(200.0f);
    // The value applies while it is dragged; the file is written once, on release.
    ImGui::SliderInt("Every (minutes)", &general.autoSaveIntervalMinutes, MIN_AUTOSAVE_MINUTES,
                     MAX_AUTOSAVE_MINUTES, "%d", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::EndDisabled();
    appliesNote("Applies now. A crash loses at most this much work.");

    ImGui::SeparatorText("Media library");
    ImGui::TextUnformatted("Default media folder:");
    ImGui::TextWrapped("%s", general.defaultMediaFolder.empty() ? "(none)" : general.defaultMediaFolder.c_str());
    ImGui::BeginDisabled(folderDialog_.isBusy());
    if (ImGui::Button("Browse...")) {
        AsyncFileDialog::Request request;
        request.kind = AsyncFileDialog::Kind::PickFolder;
        request.title = "Select the default media folder";
        request.initialPath = general.defaultMediaFolder;
        folderDialog_.open(std::move(request));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(general.defaultMediaFolder.empty());
    if (ImGui::Button("Clear")) {
        general.defaultMediaFolder.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox("Import its media at startup", &general.autoLoadMediaOnStartup);
    changed |= ImGui::Checkbox("Include subfolders", &general.recursiveFolderScan);
    ImGui::SetItemTooltip("For the startup import and for watching the folder for new files");
    appliesNote("Applies on restart. New files in the folder are imported while QUADmapper runs.");
    return changed;
}

bool PreferencesDialog::renderAudioTab(AudioEngine* audio) {
    if (!audio) {
        ImGui::TextDisabled("Sound is not available in this session.");
        return false;
    }
    // Enumerating asks the OS: once when the tab opens, and on Refresh.
    const bool refresh = ImGui::Button("Refresh");
    if (!audioTabOpen_ || refresh) audioDevices_ = audio->listDevices();
    ImGui::SameLine();
    const std::string active = audio->activeDeviceName();
    if (audio->outputState() == AudioOutputState::Failed) {
        ImGui::TextColored(kSaveError, "No audio output");
    } else if (audio->outputState() == AudioOutputState::Lost) {
        ImGui::TextColored(kSaveError, "Audio device lost: pick a device to resume");
    } else {
        ImGui::Text("Playing on: %s", active.empty() ? "no output" : active.c_str());
    }

    ImGui::SeparatorText("Output device");
    // The engine's preferred device is the setting; Application saves it when it changes
    // (the menu bar's device list does the same).
    const std::string& preferred = audio->preferredDevice();
    auto entry = [&](const char* label, const std::string& name) {
        // Choosing the current device again reopens it: how a lost device recovers.
        if (ImGui::Selectable(label, preferred == name)) audio->selectDevice(name);
    };
    entry("System default", std::string());
    for (const std::string& name : audioDevices_) entry(name.c_str(), name);
    if (!preferred.empty() &&
        std::find(audioDevices_.begin(), audioDevices_.end(), preferred) == audioDevices_.end()) {
        ImGui::TextDisabled("%s (not connected)", preferred.c_str());
    }
    appliesNote("Applies now. The same list as the menu bar's audio output.");
    return false;
}

bool PreferencesDialog::renderControlTab(control::ExternalControl* control) {
    if (!control) {
        ImGui::TextDisabled("External control is not available in this session.");
        return false;
    }
    bool changed = false;
    ImGui::SeparatorText("OSC");
    changed |= controlSettings_.renderOsc(*control, preferences_.control);
    ImGui::SeparatorText("OSC feedback");
    changed |= controlSettings_.renderFeedback(*control, preferences_.control);
    ImGui::SeparatorText("MIDI inputs");
    changed |= controlSettings_.renderMidiDevices(*control, preferences_.control);
    appliesNote("Applies now. Mappings and learning are in Window > Control.");
    return changed;
}

void PreferencesDialog::save() {
    auto result = savePreferences();
    saveError_ = result ? std::string() : result.error().message;
}

// ---- File ---------------------------------------------------------------------

std::filesystem::path PreferencesDialog::configPath() const {
    return configPath_.empty() ? AppPreferences::getDefaultConfigPath() : configPath_;
}

Result<void, Error> PreferencesDialog::loadPreferences() {
    return preferences_.loadFromFile(configPath());
}

Result<void, Error> PreferencesDialog::savePreferences() {
    return preferences_.saveToFile(configPath());
}

} // namespace quadmapper
