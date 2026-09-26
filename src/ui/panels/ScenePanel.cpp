#include "ScenePanel.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "scene/Scene.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
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
        // addScene creates the scene and makes it active. A brand-new scene is empty,
        // which blanks the canvas/Quad List and reads as "nothing happened"; seed a
        // starter quad so the new scene is immediately usable (like the first scene).
        if (Scene* s = es.addScene(es.generateUniqueSceneName())) {
            if (Quad* q = s->getQuadManager().createQuad("Quad 1")) {
                s->getQuadManager().selectQuad(q->getId());
            }
        }
        if (ctx.project) ctx.project->setModified(true);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create a new scene and make it active");

    ImGui::SameLine();

    if (ImGui::Button("Duplicate")) {
        if (es.duplicateScene(es.activeSceneId) && ctx.project) ctx.project->setModified(true);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Copy the active scene (unassigned) and make the copy active");
    }

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

    // Deferred list changes: removing, inserting or moving a scene mid-loop would
    // dangle the `scene` pointer used by the rest of the row (the output combo) and
    // skip or repeat rows. Record them and apply after the loop.
    UUID pendingDelete;
    UUID pendingDuplicate;
    int moveFrom = -1;
    int moveTo = -1;

    ImGui::BeginChild("SceneList", ImVec2(0, 0), true);

    const int sceneCount = static_cast<int>(es.scenes.size());
    for (int i = 0; i < sceneCount; ++i) {
        Scene* scene = es.scenes[static_cast<size_t>(i)].get();
        if (!scene) continue;
        bool isActive = (scene->getId() == es.activeSceneId);

        ImGui::PushID(scene);  // stable across reorder, unlike the row index

        // Active marker
        if (isActive) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), ">");
        } else {
            ImGui::TextDisabled(" ");
        }
        ImGui::SameLine();

        // Take hotkey (US-008): scenes 1-9 by list order.
        if (i < 9) {
            ImGui::TextDisabled("%d", i + 1);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Press %d to put this scene on air", i + 1);
            }
        } else {
            ImGui::TextDisabled(" ");
        }
        ImGui::SameLine();

        if (renamingId_ == scene->getId()) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputText("##rename", renameBuffer_, sizeof(renameBuffer_),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                scene->setName(renameBuffer_);
                if (ctx.project) ctx.project->setModified(true);
                renamingId_ = UUID::null();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                renamingId_ = UUID::null();
            }
        } else {
            ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick;
            if (ImGui::Selectable(scene->getName().c_str(), isActive, flags, ImVec2(180.0f, 0.0f))) {
                es.setActiveScene(scene->getId());
                if (ImGui::IsMouseDoubleClicked(0)) beginRename(*scene);
            }
            // Drag a scene onto another row to move it there.
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("SCENE_INDEX", &i, sizeof(i));
                ImGui::Text("Move %s", scene->getName().c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_INDEX")) {
                    moveFrom = *static_cast<const int*>(payload->Data);
                    moveTo = i;
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Rename", "Double-click")) beginRename(*scene);
                if (ImGui::MenuItem("Duplicate")) pendingDuplicate = scene->getId();
                if (ImGui::MenuItem("Move up", nullptr, false, i > 0)) {
                    moveFrom = i;
                    moveTo = i - 1;
                }
                if (ImGui::MenuItem("Move down", nullptr, false, i + 1 < sceneCount)) {
                    moveFrom = i;
                    moveTo = i + 1;
                }
                ImGui::Separator();
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
    if (!pendingDuplicate.isNull()) {
        if (es.duplicateScene(pendingDuplicate) && ctx.project) ctx.project->setModified(true);
    }
    if (moveFrom >= 0 && moveTo >= 0) {
        if (es.moveScene(static_cast<size_t>(moveFrom), static_cast<size_t>(moveTo)) && ctx.project) {
            ctx.project->setModified(true);
        }
    }

    ImGui::TextDisabled("Click to make active. Double-click to rename. Drag to reorder.");
    ImGui::TextDisabled("Assign each scene to an output to project it (F11).");
    ImGui::TextDisabled("Keys 1-9 put that scene on air.");
}

void ScenePanel::beginRename(const Scene& scene) {
    renamingId_ = scene.getId();
    std::strncpy(renameBuffer_, scene.getName().c_str(), sizeof(renameBuffer_) - 1);
    renameBuffer_[sizeof(renameBuffer_) - 1] = '\0';
}

}  // namespace quadmapper
