#include "MainMenuBar.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "renderer/UvTestGrid.hpp"
#include "output/OutputManager.hpp"
#include "source/SourceManager.hpp"

#include <imgui.h>

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
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
            es.pendingFileAction = FileAction::Exit;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, es.commandHistory.canUndo())) {
            es.commandHistory.undo();
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, es.commandHistory.canRedo())) {
            es.commandHistory.redo();
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
            for (const UUID& id : es.quadManager().getSelectedIds()) {
                es.quadTextures.erase(id);
                es.quadTexturePaths.erase(id);
            }
            es.quadManager().deleteSelectedQuads();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        ImGui::MenuItem("Quad List",    nullptr, &es.showQuadList);
        ImGui::MenuItem("Properties",   nullptr, &es.showProperties);
        ImGui::MenuItem("Resources",    nullptr, &es.showResources);
        ImGui::MenuItem("Video Player", nullptr, &es.showVideoPlayer);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            es.showQuadList    = true;
            es.showProperties  = true;
            es.showResources   = true;
            es.showVideoPlayer = true;
        }
        ImGui::EndMenu();
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

    // Reset all playheads (available in both modes) — restart every video (and its
    // synced audio) from the top with one click.
    ImGui::Separator();
    if (ImGui::Button("Reset Playheads")) {
        if (ctx.sourceManager) ctx.sourceManager->seekAllToStart();
    }
    ImGui::SetItemTooltip("Restart all videos/audio from the beginning");

    if (ImGui::Button("Report Bug")) {
        openUrl("https://github.com/Fiavaion/Quadmapper/issues/new?template=bug_report.md");
    }
    ImGui::SetItemTooltip("Open a new bug report on the QUADmapper GitHub");

    ImGui::EndMainMenuBar();
}

}  // namespace quadmapper
