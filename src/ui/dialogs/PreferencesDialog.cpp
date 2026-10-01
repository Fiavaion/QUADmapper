#include "PreferencesDialog.hpp"
#include "editor/ActionRegistry.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/PerformLock.hpp"
#include "audio/AudioEngine.hpp"
#include "scene/SceneFade.hpp"
#include "ui/ShortcutRouter.hpp"  // currentChordMods

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

int PreferencesDialog::windowFlags() const {
    return ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
}

void PreferencesDialog::beforeBegin(EditorContext& /*ctx*/) {
    // Sized in font units so it fits its hints at any display scale.
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(46.0f * em, 34.0f * em), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
}

void PreferencesDialog::tick(EditorContext& ctx) {
    std::vector<std::string> chosen;
    if (folderDialog_.takeResult(chosen) && !chosen.empty()) mediaFolderChosen(ctx, chosen.front());
    if (!visible) audioTabOpen_ = false;
    // A closed window or a locked show ends a capture: the keys go back to the shortcuts.
    if (!captureId_.empty() && (!visible || ctx.es.performLock.isOn())) endChordCapture(ctx);
}

void PreferencesDialog::mediaFolderChosen(EditorContext& ctx, const std::string& folder) {
    // The dialog has no owner, so it can be answered after the perform lock was taken,
    // which blocks Preferences edits (ADR-051 §4).
    if (folder.empty() || refusedByPerformLock(ctx.es, "Changing the default media folder")) return;
    preferences_.general.defaultMediaFolder = folder;
    save();
}

void PreferencesDialog::drawContent(EditorContext& ctx) {
    control::ExternalControl* control = ctx.control;
    bool changed = false;
    bool audioTab = false;
    bool keyboardTab = false;
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
        if (ImGui::BeginTabItem("Keyboard")) {
            keyboardTab = true;
            changed |= renderKeyboardTab(ctx);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    audioTabOpen_ = audioTab;
    if (!keyboardTab && !captureId_.empty()) endChordCapture(ctx);
    if (changed) save();

    if (!saveError_.empty()) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kSaveError);
        ImGui::TextWrapped("Preferences could not be saved: %s", saveError_.c_str());
        ImGui::PopStyleColor();
    }
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

bool PreferencesDialog::renderKeyboardTab(EditorContext& ctx) {
    if (!ctx.actions) {
        ImGui::TextDisabled("Shortcuts are not available in this session.");
        return false;
    }
    ActionRegistry& registry = *ctx.actions;
    bool changed = pollChordCapture(ctx);

    appliesNote("Applies now. Tab, the arrows and H stay with the canvas, and 1-9 take scenes.");
    appliesNote("A shortcut needs Ctrl, Shift, Alt or Super, except F1-F24 and Delete.");
    const bool anyRebound = !keyBindingsOf(registry).empty();
    ImGui::BeginDisabled(!anyRebound || !captureId_.empty());
    if (ImGui::Button("Reset All")) {
        registry.resetAllChords();
        keyMessage_.clear();
        changed = true;
    }
    ImGui::EndDisabled();

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("Shortcuts", 3, flags)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##edit", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableHeadersRow();
        for (const Action& action : registry.actions()) {
            ImGui::PushID(action.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(action.label.c_str());

            ImGui::TableNextColumn();
            const bool capturing = captureId_ == action.id;
            if (capturing) {
                ImGui::TextUnformatted("Press a key chord (Esc cancels)");
            } else if (action.chords.empty()) {
                ImGui::TextDisabled("(none)");
            } else {
                ImGui::TextUnformatted(chordsText(action.chords).c_str());
            }
            if (keyMessageId_ == action.id && !keyMessage_.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kSaveError);
                ImGui::TextWrapped("%s", keyMessage_.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::TableNextColumn();
            if (action.fixed) {
                ImGui::TextDisabled("Fixed");
            } else if (capturing) {
                if (ImGui::SmallButton("Cancel")) endChordCapture(ctx);
            } else {
                ImGui::BeginDisabled(!captureId_.empty());
                if (ImGui::SmallButton(action.chords.empty() ? "Set" : "Change")) {
                    beginChordCapture(ctx, action.id, 0);
                }
                if (action.chords.size() == 1) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Add")) beginChordCapture(ctx, action.id, 1);
                } else if (action.chords.size() > 1) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) {
                        // The later chords go; the first, the one the menu shows, stays.
                        registry.setChords(action.id, {action.chords.front()});
                        changed = true;
                    }
                }
                if (action.chords != action.defaultChords) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset")) {
                        registry.resetChords(action.id);
                        keyMessage_.clear();
                        changed = true;
                    }
                }
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (changed) preferences_.keyBindings = keyBindingsOf(registry);
    return changed;
}

bool PreferencesDialog::pollChordCapture(EditorContext& ctx) {
    if (captureId_.empty()) return false;
    // The canvas leaves the keys alone too: it acts only while ImGui does not want them.
    ImGui::SetNextFrameWantCaptureKeyboard(true);
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false)) continue;
        if (key == ImGuiKey_Escape) {
            endChordCapture(ctx);
            return false;
        }
        if (!isChordKey(key)) continue;  // a modifier on its own, the mouse, a gamepad

        const Action* action = ctx.actions->find(captureId_);
        if (!action) {
            endChordCapture(ctx);
            return false;
        }
        std::vector<Chord> chords = action->chords;
        const Chord chord{key, currentChordMods()};
        if (captureSlot_ < chords.size()) {
            chords[captureSlot_] = chord;
        } else {
            chords.push_back(chord);
        }
        const RebindCheck check = ctx.actions->setChords(captureId_, std::move(chords));
        keyMessageId_ = captureId_;
        keyMessage_ = rebindRefusal(check);
        endChordCapture(ctx);
        return check.result == RebindResult::Changed;
    }
    return false;
}

void PreferencesDialog::beginChordCapture(EditorContext& ctx, const std::string& id, size_t slot) {
    captureId_ = id;
    captureSlot_ = slot;
    keyMessage_.clear();
    ctx.es.capturingChord = true;
}

void PreferencesDialog::endChordCapture(EditorContext& ctx) {
    captureId_.clear();
    ctx.es.capturingChord = false;
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
