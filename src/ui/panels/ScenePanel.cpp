#include "ScenePanel.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "scene/Scene.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "project/Project.hpp"

#include <imgui.h>
#include <cstring>

namespace quadmapper {

void ScenePanel::renderContent(EditorContext& ctx) {
    renderActionButtons(ctx);
    ImGui::Separator();
    renderSceneList(ctx);
}

void ScenePanel::renderActionButtons(EditorContext& ctx) {
    EditorState& es = ctx.es;

    if (ImGui::Button("New")) {
        es.addScene(es.generateUniqueSceneName());
        if (ctx.project) ctx.project->setModified(true);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create a new scene and make it active");

    ImGui::SameLine();

    ImGui::BeginDisabled(!es.canDeleteScene());
    if (ImGui::Button("Delete")) {
        if (es.deleteScene(es.activeSceneId) && ctx.project) ctx.project->setModified(true);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(es.canDeleteScene() ? "Delete the active scene"
                                              : "Cannot delete the last scene");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(%zu scene%s)", es.scenes.size(), es.scenes.size() == 1 ? "" : "s");
}

void ScenePanel::renderSceneList(EditorContext& ctx) {
    EditorState& es = ctx.es;
    OutputManager* om = ctx.outputManager;

    // Deferred delete: removing a scene mid-loop would dangle the `scene` pointer
    // used by the rest of the row (the output combo). Record it and erase after.
    UUID pendingDelete;

    ImGui::BeginChild("SceneList", ImVec2(0, 0), true);

    for (size_t i = 0; i < es.scenes.size(); ++i) {
        Scene* scene = es.scenes[i].get();
        if (!scene) continue;
        bool isActive = (scene->getId() == es.activeSceneId);

        ImGui::PushID(static_cast<int>(i));

        // Active marker
        if (isActive) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), ">");
        } else {
            ImGui::TextDisabled(" ");
        }
        ImGui::SameLine();

        if (isRenaming_ && renamingIndex_ == static_cast<int>(i)) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputText("##rename", renameBuffer_, sizeof(renameBuffer_),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                scene->setName(renameBuffer_);
                if (ctx.project) ctx.project->setModified(true);
                isRenaming_ = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                isRenaming_ = false;
            }
        } else {
            ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick;
            if (ImGui::Selectable(scene->getName().c_str(), isActive, flags, ImVec2(180.0f, 0.0f))) {
                es.setActiveScene(scene->getId());
                if (ImGui::IsMouseDoubleClicked(0)) {
                    isRenaming_ = true;
                    renamingIndex_ = static_cast<int>(i);
                    std::strncpy(renameBuffer_, scene->getName().c_str(), sizeof(renameBuffer_) - 1);
                    renameBuffer_[sizeof(renameBuffer_) - 1] = '\0';
                }
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Rename", "Double-click")) {
                    isRenaming_ = true;
                    renamingIndex_ = static_cast<int>(i);
                    std::strncpy(renameBuffer_, scene->getName().c_str(), sizeof(renameBuffer_) - 1);
                    renameBuffer_[sizeof(renameBuffer_) - 1] = '\0';
                }
                ImGui::BeginDisabled(!es.canDeleteScene());
                if (ImGui::MenuItem("Delete")) {
                    pendingDelete = scene->getId();
                }
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
        }

        // Per-scene output assignment dropdown.
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        const UUID& assignedId = scene->getAssignedOutputId();
        const char* previewName = "(no output)";
        if (om) {
            if (const OutputWindow* assigned = om->getOutput(assignedId)) {
                previewName = assigned->getName().c_str();
            }
        }
        if (ImGui::BeginCombo("##output", previewName)) {
            if (ImGui::Selectable("(no output)", !scene->hasAssignedOutput())) {
                es.assignSceneToOutput(scene->getId(), UUID::null());
                if (ctx.project) ctx.project->setModified(true);
            }
            if (om) {
                for (const auto& output : om->getOutputs()) {
                    if (!output) continue;
                    bool selected = (output->getId() == assignedId);
                    if (ImGui::Selectable(output->getName().c_str(), selected)) {
                        es.assignSceneToOutput(scene->getId(), output->getId());
                        if (ctx.project) ctx.project->setModified(true);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::PopID();
    }

    ImGui::EndChild();

    if (!pendingDelete.isNull()) {
        if (es.deleteScene(pendingDelete) && ctx.project) ctx.project->setModified(true);
    }

    ImGui::TextDisabled("Click to make active. Double-click to rename.");
    ImGui::TextDisabled("Assign each scene to an output to project it (F11).");
}

}  // namespace quadmapper
