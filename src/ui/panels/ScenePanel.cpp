#include "ScenePanel.hpp"

#include "editor/KeyLabels.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/SliceEditing.hpp"
#include "scene/Scene.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "project/Project.hpp"
#include "ui/ConfirmPopup.hpp"

#include <imgui.h>
#include <cstring>
#include <string>

namespace quadmapper {

void ScenePanel::drawContent(EditorContext& ctx) {
    renderActionButtons(ctx);
    // Simple mode leaves the canvas size alone: routing gives a scene its
    // projector's resolution.
    if (ctx.es.uiMode == EditorState::UiMode::Advanced) renderCanvasControls(ctx);
    ImGui::Separator();
    renderSceneList(ctx);
    renderDeleteConfirm(ctx);
}

void ScenePanel::requestDelete(EditorContext& ctx, const UUID& id) {
    EditorState& es = ctx.es;
    if (!es.canDeleteScene()) return;
    const int index = findSceneIndex(es.scenes, id);
    if (index < 0) return;
    const Scene* scene = es.scenes[static_cast<size_t>(index)].get();
    const size_t quadCount = scene->getQuadManager().getQuadCount();
    bool shownOnOutput = false;
    if (ctx.outputManager) {
        for (const auto& output : ctx.outputManager->getOutputs()) {
            if (output && output->getSceneId() == id) shownOnOutput = true;
        }
    }
    std::string body = "Delete scene '" + scene->getName() + "' and its " +
                       std::to_string(quadCount) + (quadCount == 1 ? " quad" : " quads") +
                       "? This cannot be undone.";
    if (shownOnOutput) body += " Outputs showing it will go black.";
    pendingDeleteId_ = id;
    deleteConfirm_.request("Delete Scene?", std::move(body), "Delete");
}

void ScenePanel::renderDeleteConfirm(EditorContext& ctx) {
    if (!deleteConfirm_.draw()) return;
    const UUID id = pendingDeleteId_;
    pendingDeleteId_ = UUID::null();
    ctx.es.deleteScene(id, ctx.outputManager);
}

void ScenePanel::renderActionButtons(EditorContext& ctx) {
    EditorState& es = ctx.es;

    if (ImGui::Button("New")) {
        // addScene creates the scene and makes it active. A brand-new scene is empty,
        // which blanks the canvas/Quad List and reads as "nothing happened"; seed a
        // starter quad so the new scene is immediately usable (like the first scene).
        if (Scene* s = es.addScene(es.generateUniqueSceneName())) {
            // Its canvas takes the first output's resolution (ADR-047 §1).
            if (ctx.outputManager && !ctx.outputManager->getOutputs().empty()) {
                s->setCanvasSize(ctx.outputManager->getOutputs().front()->getEffectiveResolution());
            }
            if (Quad* q = s->getQuadManager().createQuad("Quad 1")) {
                s->getQuadManager().selectQuad(q->getId());
            }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create a new scene and make it active");

    ImGui::SameLine();

    if (ImGui::Button("Duplicate")) {
        es.duplicateScene(es.activeSceneId);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Copy the active scene (on no output) and make the copy active");
    }

    ImGui::SameLine();

    ImGui::BeginDisabled(!es.canDeleteScene());
    if (ImGui::Button("Delete")) {
        requestDelete(ctx, es.activeSceneId);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(es.canDeleteScene() ? "Delete the active scene"
                                              : "Cannot delete the last scene");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(%zu scene%s)", es.scenes.size(), es.scenes.size() == 1 ? "" : "s");
}

// The active scene's canvas size (ADR-047 §1): width and height in pixels, clamped to
// 1..MAX_CANVAS_DIMENSION like the loader, applied as one undo step when a field is
// left. Quads keep their normalized positions and the slices of the outputs showing
// the scene are rescaled (setSceneCanvas). Setting a size fixes it (Scene::isCanvasLocked):
// routing then never replaces it with a projector's resolution.
void ScenePanel::renderCanvasControls(EditorContext& ctx) {
    EditorState& es = ctx.es;
    Scene& scene = es.activeScene();
    const IVec2 size = scene.getCanvasSize();
    if (canvasSceneId_ != scene.getId() || !canvasEditing_) {
        canvasEdit_[0] = size.x;
        canvasEdit_[1] = size.y;
        canvasSceneId_ = scene.getId();
    }
    auto apply = [&](IVec2 newSize, bool locked) {
        setSceneCanvas(es, scene.getId(), newSize, locked);
    };

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Canvas");
    ImGui::SameLine();
    bool editing = false;
    bool applyNow = false;
    ImGui::SetNextItemWidth(64.0f);
    ImGui::InputInt("##canvasWidth", &canvasEdit_[0], 0, 0);
    editing |= ImGui::IsItemActive();
    applyNow |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetItemTooltip("Canvas width in pixels (1-%d)", MAX_CANVAS_DIMENSION);
    ImGui::SameLine();
    ImGui::TextUnformatted("x");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(64.0f);
    ImGui::InputInt("##canvasHeight", &canvasEdit_[1], 0, 0);
    editing |= ImGui::IsItemActive();
    applyNow |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetItemTooltip("Canvas height in pixels (1-%d)", MAX_CANVAS_DIMENSION);
    canvasEditing_ = editing;
    if (applyNow) {
        apply(IVec2(canvasEdit_[0], canvasEdit_[1]), true);
        canvasEditing_ = false;  // show the clamped size next frame
    }
    ImGui::SameLine();
    bool locked = scene.isCanvasLocked();
    if (ImGui::Checkbox("Fixed##canvasLocked", &locked)) apply(size, locked);
    ImGui::SetItemTooltip("On: the canvas keeps this size (set when you edit it).\n"
                          "Off: a scene one projector shows through its default slice takes\n"
                          "that projector's resolution when it is routed or resized.");
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
                if (!isBlankName(renameBuffer_)) scene->setName(renameBuffer_);
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

        // The outputs showing this scene (ADR-047: several may). Each entry toggles
        // whether that output shows it; an output shows one scene, so turning this
        // one on replaces the scene it showed.
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        std::string shownOn;
        if (om) {
            for (const auto& output : om->getOutputs()) {
                if (!output || output->getSceneId() != scene->getId()) continue;
                shownOn += (shownOn.empty() ? "" : ", ") + output->getName();
            }
        }
        if (ImGui::BeginCombo("##output", shownOn.empty() ? "(no output)" : shownOn.c_str())) {
            if (om) {
                for (const auto& output : om->getOutputs()) {
                    if (!output) continue;
                    const bool shows = output->getSceneId() == scene->getId();
                    if (ImGui::Selectable(output->getName().c_str(), shows,
                                          ImGuiSelectableFlags_DontClosePopups)) {
                        om->routeScene(*output, shows ? nullptr : scene, es.scenes);
                    }
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Outputs showing this scene");

        ImGui::PopID();
    }

    ImGui::EndChild();

    if (!pendingDelete.isNull()) {
        requestDelete(ctx, pendingDelete);
    }
    if (!pendingDuplicate.isNull()) {
        es.duplicateScene(pendingDuplicate);
    }
    if (moveFrom >= 0 && moveTo >= 0) {
        es.moveScene(static_cast<size_t>(moveFrom), static_cast<size_t>(moveTo));
    }

    ImGui::TextDisabled("Click to make active. Double-click to rename. Drag to reorder.");
    ImGui::TextDisabled("Assign each scene to an output to project it (" QM_KEY_PERFORM ").");
    ImGui::TextDisabled("Keys 1-9 put that scene on air.");
}

void ScenePanel::beginRename(const Scene& scene) {
    renamingId_ = scene.getId();
    std::strncpy(renameBuffer_, scene.getName().c_str(), sizeof(renameBuffer_) - 1);
    renameBuffer_[sizeof(renameBuffer_) - 1] = '\0';
}

}  // namespace quadmapper
