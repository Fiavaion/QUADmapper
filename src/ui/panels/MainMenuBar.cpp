#include "MainMenuBar.hpp"

#include "editor/KeyLabels.hpp"
#include "editor/ActionRegistry.hpp"
#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorServices.hpp"
#include "editor/EditorState.hpp"
#include "editor/MissingMedia.hpp"
#include "editor/PerformLock.hpp"
#include "editor/Preferences.hpp"
#include "editor/ShowTransport.hpp"
#include "editor/StatusLine.hpp"
#include "editor/UndoRouting.hpp"
#include "core/FileSystem.hpp"
#include "core/MacShell.hpp"
#include "core/Version.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "project/Project.hpp"
#include "renderer/UvTestGrid.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "source/SourceManager.hpp"
#include "audio/AudioEngine.hpp"
#include "ui/PanelHost.hpp"
#include "ui/ShortcutRouter.hpp"
#include "ui/dialogs/PreferencesDialog.hpp"
#include "ui/panels/AiPhotoPanel.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
// Open a URL in the user's default browser.
void openUrl(const char* url) {
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    quadmapper::macOpen(url);
#else
    (void)url;
#endif
}
}  // namespace

namespace quadmapper {

namespace {

constexpr ImVec4 AUDIO_WARNING_COLOR(1.0f, 0.45f, 0.35f, 1.0f);

const char* outputProblem(const AudioEngine& audio) {
    switch (audio.outputState()) {
        case AudioOutputState::Failed: return "No audio output";
        case AudioOutputState::Lost:   return "Audio device lost";
        default:                       return nullptr;
    }
}

// Open one of the program's own files or folders with its default program. A missing
// one (a damaged install) is reported on the status line, not ignored.
void openPath(EditorState& es, const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        postStatus(es, "Not found: " + utf8FromPath(path));
        return;
    }
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    macOpen(utf8FromPath(path));
#endif
}

// File > Open Recent. A choice queues the same Load that File > Open queues once its
// dialog answers, so the unsaved-changes prompt and the perform lock apply. A file that
// is gone is dropped from the list.
void renderOpenRecent(EditorContext& ctx) {
    EditorState& es = ctx.es;
    if (!ctx.prefs) return;
    std::vector<std::string>& recent = ctx.prefs->recentProjects;
    if (!ImGui::BeginMenu("Open Recent", !recent.empty() && !es.performLock.isOn())) return;

    std::string chosen;
    for (size_t i = 0; i < recent.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const std::string name = utf8FromPath(pathFromUtf8(recent[i]).filename());
        if (ImGui::MenuItem(name.c_str())) chosen = recent[i];
        ImGui::SetItemTooltip("%s", recent[i].c_str());
        ImGui::PopID();
    }
    ImGui::Separator();
    const bool clear = ImGui::MenuItem("Clear Recent");
    ImGui::EndMenu();

    if (clear) {
        recent.clear();
        if (ctx.services) ctx.services->savePreferences();
    } else if (!chosen.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(pathFromUtf8(chosen), ec)) {
            removeRecentProject(recent, chosen);
            if (ctx.services) ctx.services->savePreferences();
            postStatus(es, "Not found, removed from Open Recent: " + chosen);
        } else if (es.pendingFileAction != EditorState::FileAction::None ||
                   es.unsavedPromptFor != EditorState::FileAction::None) {
            postStatus(es, "Not opened: another file action is pending");
        } else {
            es.pendingLoadPath = chosen;
            es.pendingFileAction = EditorState::FileAction::Load;
        }
    }
}

}  // namespace

void actionMenuItem(EditorContext& ctx, const char* id, bool selected, std::optional<bool> enabled) {
    const Action* action = ctx.actions ? ctx.actions->find(id) : nullptr;
    if (!action) return;
    // The first chord: the menu shows one, the others work as well.
    const std::string chord = action->chords.empty() ? std::string() : chordLabel(action->chords.front());
    const bool canRun = (enabled ? *enabled : (!action->enabled || action->enabled(ctx))) &&
                        !performLockRefuses(ctx.es, *action);
    if (ImGui::MenuItem(action->label.c_str(), chord.empty() ? nullptr : chord.c_str(), selected,
                        canRun)) {
        ctx.actions->invoke(id, ctx, Source::Menu);
    }
}

void MainMenuBar::renderAudioDevices(AudioEngine& audio, bool menu, bool& listOpen) {
    // Enumerating asks the OS, so only when a list is opened, not every frame.
    if (!listOpen) audioDevices_ = audio.listDevices();
    listOpen = true;
    const std::string& preferred = audio.preferredDevice();
    auto entry = [&](const char* label, const std::string& name) {
        const bool selected = preferred == name;
        const bool chosen = menu ? ImGui::MenuItem(label, nullptr, selected)
                                 : ImGui::Selectable(label, selected);
        // Choosing the current device again reopens it: how a lost device recovers.
        if (chosen) audio.selectDevice(name);
    };
    entry("System default", std::string());
    for (const std::string& name : audioDevices_) entry(name.c_str(), name);
    if (!preferred.empty() &&
        std::find(audioDevices_.begin(), audioDevices_.end(), preferred) == audioDevices_.end()) {
        ImGui::TextDisabled("%s (not connected)", preferred.c_str());
    }
}

void MainMenuBar::renderAudioStrip(AudioEngine& audio) {
    ImGui::TextDisabled("Audio:");
    const bool muted = audio.masterMuted();
    if (muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.25f, 0.2f, 1.0f));
    const float muteWidth = ImGui::CalcTextSize("Muted").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    if (ImGui::Button(muted ? "Muted##master" : "Mute##master", ImVec2(muteWidth, 0.0f))) {
        audio.setMasterMuted(!muted);
    }
    if (muted) ImGui::PopStyleColor();
    ImGui::SetItemTooltip(muted ? "All sound is muted: click to unmute" : "Mute all sound");

    float percent = audio.masterVolume() * 100.0f;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(em * 6.5f);
    if (ImGui::SliderFloat("##master volume", &percent, 0.0f, 100.0f, "Vol %.0f%%")) {
        audio.setMasterVolume(percent / 100.0f);
    }
    ImGui::SetItemTooltip("Master volume of every video's sound (" QM_KEY_CTRL "+click to type a value)");

    const char* problem = outputProblem(audio);
    const std::string active = audio.activeDeviceName();
    const std::string preview = problem ? problem : (active.empty() ? "No output" : active);
    ImGui::SetNextItemWidth(em * 11.0f);
    if (problem) ImGui::PushStyleColor(ImGuiCol_Text, AUDIO_WARNING_COLOR);
    const bool open = ImGui::BeginCombo("##audio output", preview.c_str());
    if (problem) ImGui::PopStyleColor();
    if (open) {
        renderAudioDevices(audio, /*menu=*/false, audioComboOpen_);
        ImGui::EndCombo();
    } else {
        audioComboOpen_ = false;
    }
    ImGui::SetItemTooltip(problem ? "Sound is off: pick an output device to resume"
                                  : "Output device for sound (saved in the preferences)");
}

void MainMenuBar::render(EditorContext& ctx, PanelHost& panels) {
    EditorState& es = ctx.es;

    // A taller bar: ImGui sizes it from the frame padding in force at BeginMainMenuBar.
    // In font units, so it grows with Preferences > Interface size.
    const float em = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 0.8f, em * 0.5f));
    if (!ImGui::BeginMainMenuBar()) {
        ImGui::PopStyleVar();
        return;
    }

    // The perform lock (ADR-051 §4) greys out what it blocks; the session and the
    // registry refuse the same things again (a key, the Project tab, a queued action).
    const bool locked = es.performLock.isOn();

    // File actions are only requested here; Application runs them at the next frame
    // boundary, after the unsaved-changes check (ADR-046).
    if (ImGui::BeginMenu("File")) {
        using FileAction = EditorState::FileAction;
        actionMenuItem(ctx, action_id::NEW_PROJECT);
        actionMenuItem(ctx, action_id::OPEN_PROJECT);
        renderOpenRecent(ctx);
        ImGui::Separator();
        actionMenuItem(ctx, action_id::SAVE);
        actionMenuItem(ctx, action_id::SAVE_AS);
        ImGui::Separator();
        if (ImGui::MenuItem("Import 3D Object (OBJ)...", nullptr, false, !locked)) {
            es.pendingFileAction = FileAction::ImportObject;
        }
        if (ImGui::MenuItem("Relocate Missing Media...", nullptr, false,
                            !locked && !collectMissingMedia(es, ctx.sourceManager).empty())) {
            es.requestPanelOpen(EditorState::MISSING_MEDIA_PANEL_ID);
        }
        // Render the show to MP4 files for a media player (ADR-060).
        if (ImGui::MenuItem("Export Video...")) {
            es.requestPanelOpen(EditorState::EXPORT_VIDEO_PANEL_ID);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", QM_KEY_QUIT)) {
            es.pendingFileAction = FileAction::Exit;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        // While calibrating or editing a slice warp, only that output's calibration or
        // slice steps (UndoRouting.hpp): greyed out when nothing there can be undone.
        actionMenuItem(ctx, action_id::UNDO, false, canUndoStep(es));
        actionMenuItem(ctx, action_id::REDO, false, canRedoStep(es));
        ImGui::Separator();
        actionMenuItem(ctx, action_id::SELECT_ALL);
        actionMenuItem(ctx, action_id::DESELECT_ALL);
        ImGui::Separator();
        actionMenuItem(ctx, action_id::DELETE_SELECTION);
        actionMenuItem(ctx, action_id::GROUP_SELECTED);
        ImGui::Separator();
        // The stacking and mesh actions: in the menu so their keys can be found.
        if (ImGui::BeginMenu("Arrange")) {
            actionMenuItem(ctx, action_id::BRING_TO_FRONT);
            actionMenuItem(ctx, action_id::BRING_FORWARD);
            actionMenuItem(ctx, action_id::SEND_BACKWARD);
            actionMenuItem(ctx, action_id::SEND_TO_BACK);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Mesh")) {
            // Greyed out for a locked quad. The keys stay enabled so they can say
            // "<name> is locked (Quad List)".
            const Quad* primary = es.quadManager().getPrimarySelectedQuad();
            const bool quadLocked = primary && primary->isLocked();
            const auto meshItem = [&](const char* id) {
                const Action* action = ctx.actions ? ctx.actions->find(id) : nullptr;
                if (!action) return;
                actionMenuItem(ctx, id, false,
                               (!action->enabled || action->enabled(ctx)) && !quadLocked);
            };
            meshItem(action_id::TOGGLE_MESH_EDIT);
            meshItem(action_id::MESH_FINER);
            meshItem(action_id::MESH_COARSER);
            meshItem(action_id::RESET_MESH);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        // Projector calibration (ADR-052): the first output showing the active scene,
        // else the first output.
        OutputWindow* toCalibrate = nullptr;
        if (ctx.outputManager) {
            toCalibrate = ctx.outputManager->firstOutputShowing(es.activeScene().getId());
            if (!toCalibrate && !ctx.outputManager->getOutputs().empty()) {
                toCalibrate = ctx.outputManager->getOutputs().front().get();
            }
        }
        const bool calibrating = !es.calibratingOutputId.isNull();
        if (ImGui::MenuItem("Calibrate Projector...", nullptr, calibrating,
                            !locked && !es.aiScan.running && (calibrating || toCalibrate))) {
            if (calibrating) {
                es.endCalibration();
            } else {
                es.beginCalibration(*toCalibrate);
            }
        }
        ImGui::Separator();
        actionMenuItem(ctx, action_id::PREFERENCES);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        panels.drawWindowMenuItems();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Output")) {
        // Fullscreen on the projectors (F11), ticked while it is on.
        actionMenuItem(ctx, action_id::PERFORM_TOGGLE,
                       ctx.outputManager && ctx.outputManager->isInPerformMode());
        // Master blackout (ADR-048 §3): every projector output goes black.
        actionMenuItem(ctx, action_id::BLACKOUT, ctx.outputManager && ctx.outputManager->isBlackout());
        // The perform lock (ADR-051 §4): locks at once; unlocking asks first.
        actionMenuItem(ctx, action_id::PERFORM_LOCK, locked);
        if (ctx.audio) {
            ImGui::Separator();
            if (ImGui::MenuItem("Mute Audio", nullptr, ctx.audio->masterMuted())) {
                ctx.audio->setMasterMuted(!ctx.audio->masterMuted());
            }
            float percent = ctx.audio->masterVolume() * 100.0f;
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::SliderFloat("Master Volume", &percent, 0.0f, 100.0f, "%.0f%%")) {
                ctx.audio->setMasterVolume(percent / 100.0f);
            }
            if (ImGui::BeginMenu("Audio Output")) {
                renderAudioDevices(*ctx.audio, /*menu=*/true, audioMenuOpen_);
                ImGui::EndMenu();
            } else {
                audioMenuOpen_ = false;
            }
        }
        ImGui::EndMenu();
    } else {
        audioMenuOpen_ = false;
    }

    // AI Assist is a beta (Edit > Preferences > General): no AI menu until it is on.
    const bool aiBeta = ctx.prefs && ctx.prefs->general.aiAssistBeta;
    if (aiBeta && ImGui::BeginMenu("AI (Beta)")) {
        using AiAction = EditorServices::AiAction;
        EditorServices* services = ctx.services;
        auto run = [services](AiAction action) {
            if (services) services->runAi(action);
        };
        // The perform lock blocks the AI assist (it warps quads and projects patterns);
        // Clear Pattern stays, since it gives the show back.
        const bool aiAllowed = !locked;
        if (services && services->aiActive()) {
            // One camera or sidecar request at a time: the items that start one wait.
            const bool aiIdle = aiAllowed && !services->aiBusy();
            if (ImGui::MenuItem("Detect Surface", nullptr, false, aiIdle)) run(AiAction::DetectSurface);
            // ADR-053 §2: opens the AI Photo panel, which does the clicking and applying.
            const bool canSegment = services->aiCanSegment();
            if (ImGui::MenuItem("Pick Surface...", nullptr, false, aiAllowed && canSegment)) {
                es.requestPanelOpen(AiPhotoPanel::ID);
            }
            if (!canSegment) {
                ImGui::SetItemTooltip("This AI engine cannot pick surfaces: update the AI engine");
            }
            // ADR-053 §3: Gray-code patterns on one output, a camera frame of each.
            if (services->aiScanning()) {
                if (ImGui::MenuItem("Cancel Scan")) run(AiAction::CancelScan);
            } else {
                const bool canScan = services->aiCanScan();
                if (ImGui::MenuItem("Scan Output", nullptr, false, aiIdle && canScan)) {
                    run(AiAction::ScanOutput);
                }
                ImGui::SetItemTooltip(canScan
                    ? "Map the camera to the selected output (else the first enabled one): it shows "
                      "patterns fullscreen and photographs each, about 15 s"
                    : "This AI engine cannot scan: update the AI engine");
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("One-Flash")) {
                if (ImGui::MenuItem("Emit Pattern", nullptr, false, aiAllowed)) run(AiAction::EmitPattern);
                if (ImGui::MenuItem("Capture Photo", nullptr, false, aiIdle)) run(AiAction::CapturePhoto);
                if (ImGui::MenuItem("Solve & Apply", nullptr, false, aiIdle)) run(AiAction::SolveApply);
                if (ImGui::MenuItem("Anamorphic Warp", nullptr, false, aiIdle)) run(AiAction::Anamorphic);
                if (ImGui::MenuItem("Clear Pattern")) run(AiAction::ClearPattern);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::TextDisabled(services->aiBusy() ? "AI engine connected - request running..."
                                                   : "AI engine connected");
        } else {
            if (ImGui::MenuItem("Enable AI Assist", nullptr, false, aiAllowed)) run(AiAction::Enable);
            ImGui::Separator();
            ImGui::TextDisabled("AI engine not running");
        }
        ImGui::EndMenu();
    }

    // Nothing here changes the show: guides, the log and the About box.
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("User Guide")) openPath(es, resourcePath("docs/quick-start.html"));
        if (aiBeta && ImGui::MenuItem("AI Features Guide")) {
            openPath(es, resourcePath("docs/ai-features-guide.html"));
        }
        if (ImGui::MenuItem("Keyboard Shortcuts...")) {
            if (auto* preferences = dynamic_cast<PreferencesDialog*>(panels.find(PreferencesDialog::ID))) {
                preferences->showKeyboardTab();
            }
            es.requestPanelOpen(PreferencesDialog::ID);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Open Log Folder")) openPath(es, userLogDir());
        if (ImGui::MenuItem("Report a Bug...")) {
            openUrl("https://github.com/Fiavaion/Quadmapper/issues/new?template=bug_report.md");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Open-Source Licences")) {
            openPath(es, executableDir() / "THIRD-PARTY-NOTICES.txt");
        }
        if (ImGui::MenuItem("About QUADmapper")) showAbout_ = true;
        ImGui::EndMenu();
    }

    // ---- Mode toolbar (ADR-042) -------------------------------------------
    // Always-visible Simple/Advanced toggle, plus the two most common Simple-mode
    // actions surfaced as buttons so a new user never has to hunt through menus.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em * 0.8f, ImGui::GetStyle().ItemSpacing.y));
    ImGui::Separator();
    ImGui::TextDisabled("Mode:");
    const bool simple = (es.uiMode == EditorState::UiMode::Simple);
    auto modeBtn = [&](const char* label, bool active) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool clicked = ImGui::Button(label);
        if (active) ImGui::PopStyleColor();
        return clicked;
    };
    if (modeBtn("Simple", simple))    es.uiMode = EditorState::UiMode::Simple;
    if (modeBtn("Advanced", !simple)) es.uiMode = EditorState::UiMode::Advanced;

    if (simple) {
        ImGui::Separator();
        ImGui::BeginDisabled(locked || !es.quadManager().getPrimarySelectedQuad());
        const bool applyGrid = ImGui::Button("Apply Test Grid");
        ImGui::EndDisabled();
        if (applyGrid) {
            if (Quad* q = es.quadManager().getPrimarySelectedQuad()) {
                assignUvTestGrid(es.quadTextures, es.quadTexturePaths, q->getId(), 1024, 8);
                // Override any assigned video/image: the renderer resolves mediaId
                // first, so the grid (a legacy quadTextures entry) would be hidden
                // behind a bound source unless we clear it.
                q->getTexture().mediaId = UUID();
            }
        }
        ImGui::SetItemTooltip("Put a calibration grid on the selected quad, in place of its media");
        if (ImGui::Button("Send to Projector (" QM_KEY_PERFORM ")") && ctx.actions) {
            ctx.actions->invoke(action_id::PERFORM_TOGGLE, ctx, Source::Menu);
        }
        ImGui::SetItemTooltip("Toggle fullscreen output on the projector");
    }

    // The test grid on new quads (owner, 2026-11-23), in both modes: a machine
    // preference, on by default. Quads that already exist keep what they show.
    {
        ImGui::Separator();
        const bool on = es.uvGridOnNewQuads;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(on ? "Test Grid: On" : "Test Grid: Off")) es.uvGridOnNewQuads = !on;
        if (on) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("New quads start with the test grid on them (quads you have already keep theirs)");
    }

    // The show transport (editor/ShowTransport.hpp), always visible in both modes: every
    // video, locked to the master timeline or not. Space taps the same Play/Pause.
    ImGui::Separator();
    if (ctx.sourceManager) {
        SourceManager& sm = *ctx.sourceManager;
        ImGui::TextDisabled("Timeline:");
        // As wide as the wider label, so the bar does not shift when it changes.
        const float playWidth = ImGui::CalcTextSize("Pause").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::Button(showRunning(sm) ? "Pause##timeline" : "Play##timeline",
                          ImVec2(playWidth, 0.0f))) {
            toggleShow(sm);
        }
        ImGui::SetItemTooltip("Play or pause every video (Space)");
        if (ImGui::Button("Restart##timeline")) restartShow(sm);
        ImGui::SetItemTooltip("Send every video and the master timeline back to the start");

        // A fixed-width slot: proportional digits would move what follows as it counts.
        const double seconds = sm.getMasterTimeline().position(std::chrono::steady_clock::now());
        const int minutes = static_cast<int>(seconds / 60.0);
        char clock[32];
        snprintf(clock, sizeof(clock), "%02d:%04.1f", minutes, seconds - minutes * 60.0);
        const ImVec2 slot(ImGui::CalcTextSize("00:00.0").x * 1.2f, ImGui::GetFrameHeight());
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(slot);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(at.x, at.y + (slot.y - ImGui::GetFontSize()) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_Text), clock);
        ImGui::SetItemTooltip("Master timeline position");
    }

    if (ctx.audio) {
        ImGui::Separator();
        renderAudioStrip(*ctx.audio);
    }

    ImGui::PopStyleVar();  // the toolbar's item spacing
    ImGui::EndMainMenuBar();
    ImGui::PopStyleVar();  // the bar's frame padding

    if (showAbout_) {
        showAbout_ = false;
        ImGui::OpenPopup("About QUADmapper");
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About QUADmapper", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("QUADmapper %s", APP_VERSION);
        ImGui::TextDisabled("Projection mapping for live events and installations");
        ImGui::Spacing();
        ImGui::TextUnformatted("Video is decoded with FFmpeg, used under the LGPL, version 2.1 or later.");
        ImGui::TextUnformatted("Help > Open-Source Licences lists every library, font and AI model.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace quadmapper
