#include "PreferencesDialog.hpp"
#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "audio/AudioEngine.hpp"
#include "core/Logger.hpp"
#include "core/FileSystem.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#include <ShlObj.h>
#include <Windows.h>
#endif

namespace quadmapper {

namespace {

using nlohmann::json;

const ImVec4 kSaveError(1.0f, 0.35f, 0.3f, 1.0f);

// Type-checked reads: a missing key, the wrong type or an out-of-range number leaves
// `out` as it was.
void readBool(const json& section, const char* key, bool& out) {
    auto it = section.find(key);
    if (it != section.end() && it->is_boolean()) out = it->get<bool>();
}

void readInt(const json& section, const char* key, int& out, int min, int max) {
    auto it = section.find(key);
    if (it == section.end() || !it->is_number_integer()) return;
    const auto value = it->get<long long>();
    if (value >= min && value <= max) out = static_cast<int>(value);
}

void readString(const json& section, const char* key, std::string& out) {
    auto it = section.find(key);
    if (it != section.end() && it->is_string()) out = it->get<std::string>();
}

const json* sectionOf(const json& root, const char* name) {
    auto it = root.find(name);
    return it != root.end() && it->is_object() ? &*it : nullptr;
}

void appliesNote(const char* text) {
    ImGui::TextDisabled("%s", text);
}

}  // namespace

std::string AppPreferences::getDefaultConfigPath() {
    std::string configDir;

#ifdef _WIN32
    // Get APPDATA path on Windows
    wchar_t* appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
        std::wstring wpath(appDataPath);
        configDir = std::string(wpath.begin(), wpath.end());
        CoTaskMemFree(appDataPath);
    } else {
        configDir = ".";
    }
    configDir += "\\QUADmapper";
#else
    // Use XDG config home on Linux/Mac
    const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
    if (xdgConfig) {
        configDir = xdgConfig;
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            configDir = std::string(home) + "/.config";
        } else {
            configDir = ".";
        }
    }
    configDir += "/quadmapper";
#endif

    // Ensure directory exists
    FileSystem::createDirectory(configDir);

    return configDir + "/preferences.json";
}

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

    ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
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

    ImGui::SeparatorText("Auto-save");
    changed |= ImGui::Checkbox("Save unsaved work to a recovery file", &general.autoSaveEnabled);
    ImGui::BeginDisabled(!general.autoSaveEnabled);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::SliderInt("Every (minutes)", &general.autoSaveIntervalMinutes, MIN_AUTOSAVE_MINUTES,
                         MAX_AUTOSAVE_MINUTES, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        changed = true;
    }
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

Result<void, Error> PreferencesDialog::loadPreferences() {
    return loadFromFile(AppPreferences::getDefaultConfigPath());
}

Result<void, Error> PreferencesDialog::savePreferences() {
    return saveToFile(AppPreferences::getDefaultConfigPath());
}

Result<void, Error> PreferencesDialog::loadFromFile(const std::string& path) {
    auto result = FileSystem::readFile(path);
    if (!result) {
        Logger::warning("Could not load preferences from {}: {}", path, result.error().message);
        return Result<void, Error>::error(result.error());
    }

    json root;
    try {
        root = json::parse(result.value());
    } catch (const json::exception& e) {
        return Result<void, Error>::error(Error("Failed to parse preferences: " + std::string(e.what())));
    }
    if (!root.is_object() || !root.contains("version")) {
        return Result<void, Error>::error(Error("Missing version in preferences file"));
    }

    // Keys this version no longer has (older files carry many) are ignored.
    if (const json* gen = sectionOf(root, "general")) {
        GeneralPreferences& general = preferences_.general;
        readString(*gen, "defaultMediaFolder", general.defaultMediaFolder);
        readBool(*gen, "autoLoadMediaOnStartup", general.autoLoadMediaOnStartup);
        readBool(*gen, "recursiveFolderScan", general.recursiveFolderScan);
        readBool(*gen, "autoSaveEnabled", general.autoSaveEnabled);
        readInt(*gen, "autoSaveIntervalMinutes", general.autoSaveIntervalMinutes, MIN_AUTOSAVE_MINUTES,
                MAX_AUTOSAVE_MINUTES);
        readInt(*gen, "uiMode", general.uiMode, 0, 1);
    }

    if (const json* perf = sectionOf(root, "performance")) {
        readString(*perf, "audioDevice", preferences_.performance.audioDevice);
    }

    // External control (ADR-048).
    if (const json* ctl = sectionOf(root, "control")) {
        ControlPreferences& control = preferences_.control;
        readBool(*ctl, "oscEnabled", control.oscEnabled);
        readInt(*ctl, "oscPort", control.oscPort, 1, 65535);
        readString(*ctl, "oscBindAddress", control.oscBindAddress);
        auto devices = ctl->find("midiDevices");
        if (devices != ctl->end() && devices->is_array()) {
            control.midiDevices.clear();
            for (const auto& device : *devices) {
                if (device.is_string()) control.midiDevices.push_back(device.get<std::string>());
            }
        }
        readBool(*ctl, "oscFeedbackEnabled", control.oscFeedbackEnabled);
        readString(*ctl, "oscFeedbackHost", control.oscFeedbackHost);
        readInt(*ctl, "oscFeedbackPort", control.oscFeedbackPort, 0, 65535);  // 0 = the sender's port
        readBool(*ctl, "oscFeedbackUseIds", control.oscFeedbackUseIds);
    }

    Logger::info("Loaded preferences from {}", path);
    return Result<void, Error>::success();
}

Result<void, Error> PreferencesDialog::saveToFile(const std::string& path) const {
    json root;
    root["version"] = "1.0";

    const GeneralPreferences& general = preferences_.general;
    json gen;
    gen["defaultMediaFolder"] = general.defaultMediaFolder;
    gen["autoLoadMediaOnStartup"] = general.autoLoadMediaOnStartup;
    gen["recursiveFolderScan"] = general.recursiveFolderScan;
    gen["autoSaveEnabled"] = general.autoSaveEnabled;
    gen["autoSaveIntervalMinutes"] = general.autoSaveIntervalMinutes;
    gen["uiMode"] = general.uiMode;
    root["general"] = gen;

    json perf;
    perf["audioDevice"] = preferences_.performance.audioDevice;
    root["performance"] = perf;

    json ctl;
    ctl["oscEnabled"] = preferences_.control.oscEnabled;
    ctl["oscPort"] = preferences_.control.oscPort;
    ctl["oscBindAddress"] = preferences_.control.oscBindAddress;
    ctl["midiDevices"] = preferences_.control.midiDevices;
    ctl["oscFeedbackEnabled"] = preferences_.control.oscFeedbackEnabled;
    ctl["oscFeedbackHost"] = preferences_.control.oscFeedbackHost;
    ctl["oscFeedbackPort"] = preferences_.control.oscFeedbackPort;
    ctl["oscFeedbackUseIds"] = preferences_.control.oscFeedbackUseIds;
    root["control"] = ctl;

    auto result = FileSystem::writeFile(path, root.dump(2));
    if (result) {
        Logger::info("Saved preferences to {}", path);
    }
    return result;
}

} // namespace quadmapper
