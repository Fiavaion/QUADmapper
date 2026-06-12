#include "ScenePanel.hpp"
#include "scene/SceneManager.hpp"
#include "scene/Scene.hpp"
#include <imgui.h>
#include <cstring>

namespace quadmapper {

ScenePanel::ScenePanel() : Panel("Scenes") {}

void ScenePanel::render() {
    if (!visible_) return;

    ImGui::Begin(title_.c_str(), &visible_);

    if (!sceneManager_) {
        ImGui::TextDisabled("No scene manager connected");
        ImGui::End();
        return;
    }

    renderActionButtons();
    ImGui::Separator();
    renderSceneList();

    ImGui::End();
}

void ScenePanel::renderActionButtons() {
    // New Scene button
    if (ImGui::Button("New")) {
        std::string name = sceneManager_->generateUniqueSceneName();
        Scene* newScene = sceneManager_->createScene(name);
        if (newScene) {
            sceneManager_->setActiveScene(newScene->getId());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create a new scene");
    }

    ImGui::SameLine();

    // Duplicate button
    if (ImGui::Button("Duplicate")) {
        const Scene* activeScene = sceneManager_->getActiveScene();
        if (activeScene) {
            Scene* duplicate = sceneManager_->duplicateScene(activeScene->getId());
            if (duplicate) {
                sceneManager_->setActiveScene(duplicate->getId());
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Duplicate the active scene");
    }

    ImGui::SameLine();

    // Delete button (disabled if only one scene)
    ImGui::BeginDisabled(!sceneManager_->canDeleteScene());
    if (ImGui::Button("Delete")) {
        const Scene* activeScene = sceneManager_->getActiveScene();
        if (activeScene && sceneManager_->canDeleteScene()) {
            sceneManager_->deleteScene(activeScene->getId());
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (sceneManager_->canDeleteScene()) {
            ImGui::SetTooltip("Delete the active scene");
        } else {
            ImGui::SetTooltip("Cannot delete the last scene");
        }
    }

    // Scene count display
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu scenes)", sceneManager_->getSceneCount());
}

void ScenePanel::renderSceneList() {
    const auto& scenes = sceneManager_->getScenes();
    const UUID& activeId = sceneManager_->getActiveSceneId();

    ImGui::Text("Scenes:");
    ImGui::BeginChild("SceneList", ImVec2(0, 0), true);

    for (size_t i = 0; i < scenes.size(); ++i) {
        Scene* scene = scenes[i].get();
        bool isActive = (scene->getId() == activeId);

        ImGui::PushID(static_cast<int>(i));

        // Keyboard shortcut hint (1-9)
        if (i < 9) {
            ImGui::TextDisabled("%zu", i + 1);
            ImGui::SameLine();
        } else {
            ImGui::TextDisabled(" ");
            ImGui::SameLine();
        }

        // Active indicator
        if (isActive) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), ">");
        } else {
            ImGui::TextDisabled(" ");
        }
        ImGui::SameLine();

        // Check if we are renaming this scene
        if (isRenaming_ && renamingIndex_ == i) {
            ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##rename", renameBuffer_, sizeof(renameBuffer_),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                scene->setName(renameBuffer_);
                isRenaming_ = false;
            }
            // Cancel rename on escape or click elsewhere
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0))) {
                isRenaming_ = false;
            }
        } else {
            // Scene name as selectable
            ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick;
            if (ImGui::Selectable(scene->getName().c_str(), isActive, flags)) {
                // Single click: select/activate scene
                sceneManager_->setActiveScene(scene->getId());

                // Double click: start renaming
                if (ImGui::IsMouseDoubleClicked(0)) {
                    isRenaming_ = true;
                    renamingIndex_ = i;
                    std::strncpy(renameBuffer_, scene->getName().c_str(), sizeof(renameBuffer_) - 1);
                    renameBuffer_[sizeof(renameBuffer_) - 1] = '\0';
                }
            }

            // Drag source for reordering
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                dragSourceIndex_ = static_cast<int>(i);
                ImGui::SetDragDropPayload("SCENE_REORDER", &i, sizeof(size_t));
                ImGui::Text("Move: %s", scene->getName().c_str());
                ImGui::EndDragDropSource();
            }

            // Drop target for reordering
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_REORDER")) {
                    size_t sourceIndex = *static_cast<const size_t*>(payload->Data);
                    if (sourceIndex != i) {
                        sceneManager_->moveScene(sourceIndex, i);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Context menu
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Rename", "Double-click")) {
                    isRenaming_ = true;
                    renamingIndex_ = i;
                    std::strncpy(renameBuffer_, scene->getName().c_str(), sizeof(renameBuffer_) - 1);
                    renameBuffer_[sizeof(renameBuffer_) - 1] = '\0';
                }
                if (ImGui::MenuItem("Duplicate")) {
                    Scene* duplicate = sceneManager_->duplicateScene(scene->getId());
                    if (duplicate) {
                        sceneManager_->setActiveScene(duplicate->getId());
                    }
                }
                ImGui::Separator();
                ImGui::BeginDisabled(!sceneManager_->canDeleteScene());
                if (ImGui::MenuItem("Delete")) {
                    sceneManager_->deleteScene(scene->getId());
                }
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
        }

        ImGui::PopID();
    }

    ImGui::EndChild();

    // Help text
    ImGui::TextDisabled("Tip: Double-click to rename. Drag to reorder.");
    ImGui::TextDisabled("Press 1-9 to switch scenes (keyboard shortcuts).");
}

void ScenePanel::handleDragDrop(size_t sourceIndex) {
    // This is called from the drag-drop handling in renderSceneList
    // The actual move is done via sceneManager_->moveScene()
    (void)sourceIndex;
}

} // namespace quadmapper
