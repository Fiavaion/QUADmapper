#include "MainMenuBar.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>

namespace quadmapper {

void MainMenuBar::render(EditorContext& ctx) {
    EditorState& es = ctx.es;

    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Project", "Ctrl+N")) {
            if (onNewProject_) onNewProject_();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
            if (onSaveProject_) onSaveProject_();
        }
        if (ImGui::MenuItem("Load Project", "Ctrl+O")) {
            if (onLoadProject_) onLoadProject_();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
            glfwSetWindowShouldClose(ctx.window, true);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Select All", "Ctrl+A")) {
            es.quadManager.selectAll();
        }
        if (ImGui::MenuItem("Deselect All", "Escape")) {
            es.quadManager.deselectAll();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete Selected", "Delete", false, es.quadManager.hasSelection())) {
            for (const UUID& id : es.quadManager.getSelectedIds()) {
                es.quadTextures.erase(id);
                es.quadTexturePaths.erase(id);
            }
            es.quadManager.deleteSelectedQuads();
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

    ImGui::EndMainMenuBar();
}

}  // namespace quadmapper
