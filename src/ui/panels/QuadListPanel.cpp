#include "QuadListPanel.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "scene/QuadGroup.hpp"
#include "scene/Scene.hpp"
#include "renderer/Texture.hpp"
#include "core/UUID.hpp"
#include "project/Project.hpp"

#include <imgui.h>
#include <unordered_set>
#include <string>
#include <memory>

namespace quadmapper {

void QuadListPanel::render(EditorContext& ctx) {
    EditorState& es = ctx.es;

    if (!es.showQuadList) return;

    ImGui::Begin("Quad List");

    ImGuiIO& io = ImGui::GetIO();

    auto& quadManager         = es.quadManager;
    auto& scene               = es.scene;
    auto& quadTextures        = es.quadTextures;
    auto& quadTexturePaths    = es.quadTexturePaths;
    auto& currentTexturePath  = es.currentTexturePath;
    auto& dragDropQuadId      = es.dragDropQuadId;
    auto& currentSelectionType = es.currentSelectionType;

    // Add Quad button
    if (ImGui::Button("+ Add Quad")) {
        Quad* newQuad = quadManager.createQuad();
        quadManager.selectQuad(newQuad->getId());
        currentTexturePath[0] = '\0';
        ctx.project->setModified(true);
        // Switch to Quad properties and clear other panel selections (deliberate
        // cross-panel coordination via EditorState — see ADR-0034 / TD-02 contract).
        currentSelectionType = EditorState::SelectionType::Quad;
        es.mediaBinPanel.clearSelection();
        es.outputPanel.clearSelection();
    }

    ImGui::SameLine();

    // Delete button
    if (ImGui::Button("Delete") && quadManager.hasSelection()) {
        for (const UUID& id : quadManager.getSelectedIds()) {
            quadTextures.erase(id);
            quadTexturePaths.erase(id);
        }
        quadManager.deleteSelectedQuads();
        currentTexturePath[0] = '\0';
        ctx.project->setModified(true);
    }

    ImGui::SameLine();

    // Duplicate button
    if (ImGui::Button("Duplicate") && quadManager.getPrimarySelectedQuad()) {
        auto primaryId = quadManager.getPrimarySelectedId();
        if (primaryId.has_value()) {
            Quad* newQuad = quadManager.duplicateQuad(*primaryId);
            if (newQuad) {
                // Duplicate texture too
                auto texIt = quadTextures.find(*primaryId);
                if (texIt != quadTextures.end()) {
                    auto pathIt = quadTexturePaths.find(*primaryId);
                    if (pathIt != quadTexturePaths.end()) {
                        quadTexturePaths[newQuad->getId()] = pathIt->second;
                        auto newTex = std::make_unique<Texture>();
                        if (newTex->loadFromFile(pathIt->second)) {
                            quadTextures[newQuad->getId()] = std::move(newTex);
                        }
                    }
                }
                ctx.project->setModified(true);
            }
        }
    }

    // Group creation button (enabled when multiple quads selected)
    if (quadManager.getSelectionCount() >= 2) {
        ImGui::SameLine();
        if (ImGui::Button("Group")) {
            QuadGroup* newGroup = scene.createGroupFromSelection();
            if (newGroup) {
                ctx.project->setModified(true);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Create group from selected quads (Ctrl+G)");
        }
    }

    ImGui::Separator();
    ImGui::Text("Quads: %zu  Groups: %zu (top = front)", quadManager.getQuadCount(), scene.getGroupCount());
    ImGui::Separator();

    // Helper lambda to render a single quad item
    auto renderQuadItem = [&](const Quad* quad, bool indented = false) {
        bool isSelected = quadManager.isSelected(quad->getId());

        if (indented) {
            ImGui::Indent(20.0f);
        }

        ImGui::PushID(quad->getId().toString().c_str());

        // Z-order display
        ImGui::Text("[%d]", quad->getZOrder());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Z-Order: %d (higher = on top)", quad->getZOrder());
        }
        ImGui::SameLine();

        // Up/Down arrow buttons for z-order
        bool isFirst = (quad->getZOrder() == static_cast<int>(quadManager.getQuadCount()) - 1);
        bool isLast = (quad->getZOrder() == 0);

        ImGui::BeginDisabled(isFirst);
        if (ImGui::SmallButton("^")) {
            quadManager.moveQuadDown(quad->getId());
            ctx.project->setModified(true);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Bring Forward (Ctrl+])");
        }
        ImGui::SameLine();

        ImGui::BeginDisabled(isLast);
        if (ImGui::SmallButton("v")) {
            quadManager.moveQuadUp(quad->getId());
            ctx.project->setModified(true);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Send Backward (Ctrl+[)");
        }
        ImGui::SameLine();

        // Visibility checkbox
        bool visible = quad->isVisible();
        if (ImGui::Checkbox("##visible", &visible)) {
            Quad* mutableQuad = quadManager.getQuad(quad->getId());
            if (mutableQuad) {
                mutableQuad->setVisible(visible);
                ctx.project->setModified(true);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Visible");
        }
        ImGui::SameLine();

        // Lock checkbox
        bool locked = quad->isLocked();
        if (ImGui::Checkbox("##locked", &locked)) {
            Quad* mutableQuad = quadManager.getQuad(quad->getId());
            if (mutableQuad) {
                mutableQuad->setLocked(locked);
                ctx.project->setModified(true);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Locked");
        }
        ImGui::SameLine();

        // Selectable quad name
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (isSelected) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        ImGui::TreeNodeEx(quad->getName().c_str(), flags);

        // Drag source for moving quads between groups
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            dragDropQuadId = quad->getId();
            ImGui::SetDragDropPayload("QUAD_DND", &dragDropQuadId, sizeof(UUID));
            ImGui::Text("Move: %s", quad->getName().c_str());
            ImGui::EndDragDropSource();
        }

        if (ImGui::IsItemClicked()) {
            if (io.KeyCtrl) {
                quadManager.toggleSelection(quad->getId());
            } else if (io.KeyShift && quadManager.getPrimarySelectedId().has_value()) {
                quadManager.selectRange(*quadManager.getPrimarySelectedId(), quad->getId());
            } else {
                quadManager.selectQuad(quad->getId());
            }

            // Switch to Quad properties and clear other selections
            currentSelectionType = EditorState::SelectionType::Quad;
            es.mediaBinPanel.clearSelection();
            es.outputPanel.clearSelection();

            auto pathIt = quadTexturePaths.find(quad->getId());
            if (pathIt != quadTexturePaths.end()) {
                strncpy(currentTexturePath, pathIt->second.c_str(), sizeof(currentTexturePath) - 1);
                currentTexturePath[sizeof(currentTexturePath) - 1] = '\0';
            } else {
                currentTexturePath[0] = '\0';
            }
        }

        // Context menu for z-order and groups
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Bring to Front", "Ctrl+Shift+]")) {
                quadManager.moveQuadToBottom(quad->getId());
                ctx.project->setModified(true);
            }
            if (ImGui::MenuItem("Bring Forward", "Ctrl+]")) {
                quadManager.moveQuadDown(quad->getId());
                ctx.project->setModified(true);
            }
            if (ImGui::MenuItem("Send Backward", "Ctrl+[")) {
                quadManager.moveQuadUp(quad->getId());
                ctx.project->setModified(true);
            }
            if (ImGui::MenuItem("Send to Back", "Ctrl+Shift+[")) {
                quadManager.moveQuadToTop(quad->getId());
                ctx.project->setModified(true);
            }

            // Group submenu
            if (scene.getGroupCount() > 0) {
                ImGui::Separator();
                if (ImGui::BeginMenu("Add to Group")) {
                    for (const auto& groupPtr : scene.getGroups()) {
                        QuadGroup* grp = groupPtr.get();
                        bool inGroup = grp->hasMember(quad->getId());
                        if (ImGui::MenuItem(grp->getName().c_str(), nullptr, inGroup)) {
                            if (inGroup) {
                                scene.removeQuadFromGroup(grp->getId(), quad->getId());
                            } else {
                                scene.addQuadToGroup(grp->getId(), quad->getId());
                            }
                            ctx.project->setModified(true);
                        }
                    }
                    ImGui::EndMenu();
                }
            }

            ImGui::EndPopup();
        }

        ImGui::PopID();

        if (indented) {
            ImGui::Unindent(20.0f);
        }
    };

    // Track which quads are shown in groups (to avoid duplicates)
    std::unordered_set<UUID> quadsShownInGroups;

    // Render groups first
    for (const auto& groupPtr : scene.getGroups()) {
        QuadGroup* group = groupPtr.get();
        ImGui::PushID(group->getId().toString().c_str());

        // Group visibility checkbox
        bool groupVisible = group->isVisible();
        if (ImGui::Checkbox("##grpVisible", &groupVisible)) {
            scene.applyGroupVisibility(group->getId(), groupVisible);
            ctx.project->setModified(true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Toggle group visibility");
        }
        ImGui::SameLine();

        // Group lock checkbox
        bool groupLocked = group->isLocked();
        if (ImGui::Checkbox("##grpLocked", &groupLocked)) {
            scene.applyGroupLock(group->getId(), groupLocked);
            ctx.project->setModified(true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Toggle group lock");
        }
        ImGui::SameLine();

        // Group tree node with expand/collapse
        ImGuiTreeNodeFlags groupFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (!group->isCollapsed()) {
            groupFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        std::string groupLabel = group->getName() + " (" + std::to_string(group->getMemberCount()) + ")";
        bool nodeOpen = ImGui::TreeNodeEx(groupLabel.c_str(), groupFlags);

        // Update collapsed state based on tree node state
        group->setCollapsed(!nodeOpen);

        // Context menu for group
        if (ImGui::BeginPopupContextItem()) {
            strncpy(groupNameBuf_, group->getName().c_str(), sizeof(groupNameBuf_) - 1);
            groupNameBuf_[sizeof(groupNameBuf_) - 1] = '\0';

            if (ImGui::InputText("Rename", groupNameBuf_, sizeof(groupNameBuf_), ImGuiInputTextFlags_EnterReturnsTrue)) {
                group->setName(groupNameBuf_);
                ctx.project->setModified(true);
                ImGui::CloseCurrentPopup();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Delete Group")) {
                scene.deleteGroup(group->getId());
                ctx.project->setModified(true);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Removes group but keeps quads");
            }

            ImGui::EndPopup();
        }

        // Drop target for adding quads to group
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("QUAD_DND")) {
                UUID droppedQuadId = *(const UUID*)payload->Data;
                scene.addQuadToGroup(group->getId(), droppedQuadId);
                ctx.project->setModified(true);
            }
            ImGui::EndDragDropTarget();
        }

        // Render member quads if node is open
        if (nodeOpen) {
            for (const UUID& quadId : group->getMemberQuadIds()) {
                const Quad* quad = quadManager.getQuad(quadId);
                if (quad) {
                    quadsShownInGroups.insert(quadId);
                    renderQuadItem(quad, true);
                }
            }
            ImGui::TreePop();
        } else {
            // Even if collapsed, mark quads as shown in groups
            for (const UUID& quadId : group->getMemberQuadIds()) {
                quadsShownInGroups.insert(quadId);
            }
        }

        ImGui::PopID();
    }

    // Render ungrouped quads
    bool hasUngroupedQuads = false;
    for (const auto& quadPtr : quadManager.getQuads()) {
        if (quadsShownInGroups.find(quadPtr->getId()) == quadsShownInGroups.end()) {
            hasUngroupedQuads = true;
            break;
        }
    }

    if (hasUngroupedQuads) {
        if (scene.getGroupCount() > 0) {
            ImGui::Separator();
            ImGui::TextDisabled("Ungrouped:");
        }

        // Sort ungrouped quads by z-order descending
        auto sortedQuads = quadManager.getQuadsByZOrderDescending();
        for (const Quad* quad : sortedQuads) {
            if (quadsShownInGroups.find(quad->getId()) == quadsShownInGroups.end()) {
                renderQuadItem(quad, false);
            }
        }
    }

    // Drop target for removing quads from all groups (drop on empty space)
    if (scene.getGroupCount() > 0) {
        ImGui::Dummy(ImVec2(0, 20));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("QUAD_DND")) {
                UUID droppedQuadId = *(const UUID*)payload->Data;
                for (auto& grp : scene.getGroups()) {
                    grp->removeMember(droppedQuadId);
                }
                ctx.project->setModified(true);
            }
            ImGui::EndDragDropTarget();
        }
    }

    ImGui::End();
}

}  // namespace quadmapper
