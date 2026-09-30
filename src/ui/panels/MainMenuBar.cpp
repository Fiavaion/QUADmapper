#include "MainMenuBar.hpp"

#include "editor/ActionRegistry.hpp"
#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorServices.hpp"
#include "editor/EditorState.hpp"
#include "editor/MissingMedia.hpp"
#include "editor/PerformLock.hpp"
#include "editor/UndoRouting.hpp"
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

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
// Open a URL in the user's default browser (Win32; no-op elsewhere).
void openUrl(const char* url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
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

}  // namespace

void actionMenuItem(EditorContext& ctx, const char* id, bool selected, std::optional<bool> enabled) {
    const Action* action = ctx.actions ? ctx.actions->find(id) : nullptr;
    if (!action) return;
    // The first chord: the menu shows one, the others work as well.
    const std::string chord = action->chords.empty() ? std::string() : chordText(action->chords.front());
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
    if (ImGui::Button(muted ? "Muted##master" : "Mute##master")) {
        audio.setMasterMuted(!muted);
    }
    if (muted) ImGui::PopStyleColor();
    ImGui::SetItemTooltip(muted ? "All sound is muted: click to unmute" : "Mute all sound");

    float percent = audio.masterVolume() * 100.0f;
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::SliderFloat("##master volume", &percent, 0.0f, 100.0f, "Vol %.0f%%")) {
        audio.setMasterVolume(percent / 100.0f);
    }
    ImGui::SetItemTooltip("Master volume of every video's sound (Ctrl+click to type a value)");

    const char* problem = outputProblem(audio);
    const std::string active = audio.activeDeviceName();
    const std::string preview = problem ? problem : (active.empty() ? "No output" : active);
    ImGui::SetNextItemWidth(160.0f);
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

    if (!ImGui::BeginMainMenuBar()) {
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
        ImGui::Separator();
        actionMenuItem(ctx, action_id::SAVE);
        actionMenuItem(ctx, action_id::SAVE_AS);
        ImGui::Separator();
        if (ImGui::MenuItem("Import 3D Object (OBJ)...", nullptr, false, !locked)) {
            es.pendingFileAction = FileAction::ImportObject;
        }
        if (ImGui::MenuItem("Relocate Missing Media...", nullptr, false,
                            !locked && !collectMissingMedia(es).empty())) {
            es.requestPanelOpen(EditorState::MISSING_MEDIA_PANEL_ID);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
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
                            !locked && (calibrating || toCalibrate))) {
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

    if (ImGui::BeginMenu("AI")) {
        using AiAction = EditorServices::AiAction;
        EditorServices* services = ctx.services;
        auto run = [services](AiAction action) {
            if (services) services->runAi(action);
        };
        // The perform lock blocks the AI assist (it warps quads and projects patterns);
        // Clear Pattern stays, since it gives the show back.
        const bool aiAllowed = !locked;
        if (services && services->aiActive()) {
            if (ImGui::MenuItem("Detect Surface (stub)", nullptr, false, aiAllowed)) run(AiAction::DetectSurface);
            ImGui::Separator();
            if (ImGui::BeginMenu("One-Flash")) {
                if (ImGui::MenuItem("Emit Pattern", nullptr, false, aiAllowed)) run(AiAction::EmitPattern);
                if (ImGui::MenuItem("Capture Photo", nullptr, false, aiAllowed)) run(AiAction::CapturePhoto);
                if (ImGui::MenuItem("Solve & Apply", nullptr, false, aiAllowed)) run(AiAction::SolveApply);
                if (ImGui::MenuItem("Anamorphic Warp", nullptr, false, aiAllowed)) run(AiAction::Anamorphic);
                if (ImGui::MenuItem("Clear Pattern")) run(AiAction::ClearPattern);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Sidecar connected");
        } else {
            if (ImGui::MenuItem("Enable AI Assist", nullptr, false, aiAllowed)) run(AiAction::Enable);
            ImGui::Separator();
            ImGui::TextDisabled("Sidecar not running");
        }
        ImGui::EndMenu();
    }

    // ---- Mode toolbar (ADR-042) -------------------------------------------
    // Always-visible Simple/Advanced toggle, plus the two most common Simple-mode
    // actions surfaced as buttons so a new user never has to hunt through menus.
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
        ImGui::BeginDisabled(locked);
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
        ImGui::SetItemTooltip("Put a calibration grid on the selected quad");
        if (ImGui::Button("Send to Projector (F11)") && ctx.actions) {
            ctx.actions->invoke(action_id::PERFORM_TOGGLE, ctx, Source::Menu);
        }
        ImGui::SetItemTooltip("Toggle fullscreen output on the projector");
    }

    // Master timeline transport (ADR-050 §3), always visible in both modes: the clock
    // every "Lock to master" source follows. Here beside Reset Playheads, the other
    // show-wide transport action, so a locked show is started and stopped in one place.
    ImGui::Separator();
    if (ctx.sourceManager) {
        SourceManager& sm = *ctx.sourceManager;
        const PlaybackClock& master = sm.getMasterTimeline();
        ImGui::TextDisabled("Timeline:");
        if (ImGui::Button(master.running() ? "Pause##timeline" : "Play##timeline")) {
            if (master.running()) sm.pauseMasterTimeline(); else sm.playMasterTimeline();
        }
        ImGui::SetItemTooltip("Play or pause the master timeline that locked videos follow");
        if (ImGui::Button("Restart##timeline")) sm.restartMasterTimeline();
        ImGui::SetItemTooltip("Send the master timeline, and every locked video, back to the start");
        const double seconds = master.position(std::chrono::steady_clock::now());
        const int minutes = static_cast<int>(seconds / 60.0);
        ImGui::Text("%02d:%04.1f", minutes, seconds - minutes * 60.0);
        ImGui::SetItemTooltip("Master timeline position");
    }

    if (ctx.audio) {
        ImGui::Separator();
        renderAudioStrip(*ctx.audio);
    }

    // Reset all playheads (available in both modes) — restart every video (and its
    // synced audio) from the top with one click.
    ImGui::Separator();
    if (ImGui::Button("Reset Playheads")) {
        if (ctx.sourceManager) ctx.sourceManager->seekAllToStart();
    }
    ImGui::SetItemTooltip("Restart all videos/audio and the master timeline from the beginning");

    if (ImGui::Button("Report Bug")) {
        openUrl("https://github.com/Fiavaion/Quadmapper/issues/new?template=bug_report.md");
    }
    ImGui::SetItemTooltip("Open a new bug report on the QUADmapper GitHub");

    ImGui::EndMainMenuBar();
}

}  // namespace quadmapper
