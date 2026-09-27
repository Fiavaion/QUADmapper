#include "MainMenuBar.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "app/MissingMedia.hpp"
#include "app/UndoRouting.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "project/Project.hpp"
#include "renderer/UvTestGrid.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "source/SourceManager.hpp"
#include "audio/AudioEngine.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>

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

void MainMenuBar::renderAudioStrip(EditorContext& ctx, AudioEngine& audio) {
    ImGui::TextDisabled("Audio:");
    const bool muted = audio.masterMuted();
    if (muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.25f, 0.2f, 1.0f));
    if (ImGui::Button(muted ? "Muted##master" : "Mute##master")) {
        audio.setMasterMuted(!muted);
        if (ctx.project) ctx.project->setModified(true);
    }
    if (muted) ImGui::PopStyleColor();
    ImGui::SetItemTooltip(muted ? "All sound is muted: click to unmute" : "Mute all sound");

    float percent = audio.masterVolume() * 100.0f;
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::SliderFloat("##master volume", &percent, 0.0f, 100.0f, "Vol %.0f%%")) {
        audio.setMasterVolume(percent / 100.0f);
        if (ctx.project) ctx.project->setModified(true);
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

void MainMenuBar::render(EditorContext& ctx) {
    EditorState& es = ctx.es;

    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    // File actions are only requested here; Application runs them at the next frame
    // boundary, after the unsaved-changes check (ADR-046).
    if (ImGui::BeginMenu("File")) {
        using FileAction = EditorState::FileAction;
        if (ImGui::MenuItem("New Project")) {
            es.pendingFileAction = FileAction::New;
        }
        if (ImGui::MenuItem("Open Project...")) {
            es.pendingFileAction = FileAction::Open;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
            es.pendingFileAction = FileAction::Save;
        }
        if (ImGui::MenuItem("Save Project As...")) {
            es.pendingFileAction = FileAction::SaveAs;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Import 3D Object (OBJ)...")) {
            es.pendingFileAction = FileAction::ImportObject;
        }
        if (ImGui::MenuItem("Relocate Missing Media...", nullptr, false,
                            !collectMissingMedia(es).empty())) {
            es.showMissingMedia = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
            es.pendingFileAction = FileAction::Exit;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        // While calibrating, only that output's calibration steps (UndoRouting.hpp).
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndoStep(es))) {
            if (undoStep(es) && ctx.project) ctx.project->setModified(true);
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedoStep(es))) {
            if (redoStep(es) && ctx.project) ctx.project->setModified(true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A")) {
            es.quadManager().selectAll();
        }
        if (ImGui::MenuItem("Deselect All", "Escape")) {
            es.quadManager().deselectAll();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete Selected", "Delete", false, es.quadManager().hasSelection())) {
            es.deleteSelectedQuads();
            if (ctx.project) ctx.project->setModified(true);
        }
        ImGui::Separator();
        // Projector calibration (ADR-052): the output the active scene is routed to,
        // else the first output.
        OutputWindow* toCalibrate = nullptr;
        if (ctx.outputManager) {
            toCalibrate = ctx.outputManager->getOutput(es.activeScene().getAssignedOutputId());
            if (!toCalibrate && !ctx.outputManager->getOutputs().empty()) {
                toCalibrate = ctx.outputManager->getOutputs().front().get();
            }
        }
        const bool calibrating = !es.calibratingOutputId.isNull();
        if (ImGui::MenuItem("Calibrate Projector...", nullptr, calibrating,
                            calibrating || toCalibrate != nullptr)) {
            if (calibrating) {
                es.endCalibration();
            } else {
                es.beginCalibration(toCalibrate->getId(), toCalibrate->getCalibrationPoints(),
                                    toCalibrate->getProjector());
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Preferences...", "Ctrl+,")) es.showPreferences = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        ImGui::MenuItem("Quad List",    nullptr, &es.showQuadList);
        ImGui::MenuItem("Properties",   nullptr, &es.showProperties);
        ImGui::MenuItem("Resources",    nullptr, &es.showResources);
        ImGui::MenuItem("Video Player", nullptr, &es.showVideoPlayer);
        ImGui::MenuItem("Control (OSC / MIDI)", nullptr, &es.showControl);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            es.showQuadList    = true;
            es.showProperties  = true;
            es.showResources   = true;
            es.showVideoPlayer = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Output")) {
        // Master blackout (ADR-048 §3): every projector output goes black.
        const bool blackout = ctx.outputManager && ctx.outputManager->isBlackout();
        if (ImGui::MenuItem("Blackout", "Ctrl+B", blackout, ctx.outputManager != nullptr)) {
            ctx.outputManager->toggleBlackout();
        }
        if (ctx.audio) {
            ImGui::Separator();
            if (ImGui::MenuItem("Mute Audio", nullptr, ctx.audio->masterMuted())) {
                ctx.audio->setMasterMuted(!ctx.audio->masterMuted());
                if (ctx.project) ctx.project->setModified(true);
            }
            float percent = ctx.audio->masterVolume() * 100.0f;
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::SliderFloat("Master Volume", &percent, 0.0f, 100.0f, "%.0f%%")) {
                ctx.audio->setMasterVolume(percent / 100.0f);
                if (ctx.project) ctx.project->setModified(true);
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
        const bool active = aiActiveQuery_ && aiActiveQuery_();
        if (active) {
            if (ImGui::MenuItem("Detect Surface (stub)")) {
                if (onDetectSurface_) onDetectSurface_();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("One-Flash")) {
                if (ImGui::MenuItem("Emit Pattern")) {
                    if (onEmitPattern_) onEmitPattern_();
                }
                if (ImGui::MenuItem("Capture Photo")) {
                    if (onCapturePhoto_) onCapturePhoto_();
                }
                if (ImGui::MenuItem("Solve & Apply")) {
                    if (onSolveApply_) onSolveApply_();
                }
                if (ImGui::MenuItem("Anamorphic Warp")) {
                    if (onAnamorphic_) onAnamorphic_();
                }
                if (ImGui::MenuItem("Clear Pattern")) {
                    if (onClearPattern_) onClearPattern_();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Sidecar connected");
        } else {
            if (ImGui::MenuItem("Enable AI Assist")) {
                if (onEnableAi_) onEnableAi_();
            }
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
        if (ImGui::Button("Apply Test Grid")) {
            if (Quad* q = es.quadManager().getPrimarySelectedQuad()) {
                assignUvTestGrid(es.quadTextures, es.quadTexturePaths, q->getId(), 1024, 8);
                // Override any assigned video/image: the renderer resolves mediaId
                // first, so the grid (a legacy quadTextures entry) would be hidden
                // behind a bound source unless we clear it.
                q->getTexture().mediaId = UUID();
            }
        }
        ImGui::SetItemTooltip("Put a calibration grid on the selected quad");
        if (ImGui::Button("Send to Projector (F11)")) {
            if (ctx.outputManager) ctx.outputManager->handleF11Key();
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
        renderAudioStrip(ctx, *ctx.audio);
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
