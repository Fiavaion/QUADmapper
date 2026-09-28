#include "QuadListPanel.hpp"

#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/MediaAssign.hpp"
#include "editor/QuadSelection.hpp"
#include "editor/MeshLoads.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "scene/QuadGroup.hpp"
#include "scene/Commands.hpp"
#include "scene/Scene.hpp"
#include "scene/MeshObject.hpp"
#include "media/MediaLibrary.hpp"
#include "media/MediaItem.hpp"
#include "core/UUID.hpp"
#include "project/Project.hpp"

#include <imgui.h>
#include <unordered_set>
#include <string>
#include <memory>
#include <cstring>
#include <cfloat>
#include <cstdio>

namespace quadmapper {

void QuadListPanel::render(EditorContext& ctx) {
    EditorState& es = ctx.es;

    if (!es.showQuadList) return;

    ImGui::Begin("Quad List");

    ImGuiIO& io = ImGui::GetIO();

    auto& quadManager         = es.quadManager();
    auto& scene               = es.activeScene();
    auto& quadTexturePaths    = es.quadTexturePaths;
    auto& dragDropQuadId      = es.dragDropQuadId;

    // Add Quad button
    if (ImGui::Button("+ Add Quad")) {
        Quad* newQuad = quadManager.createQuad();
        quadManager.selectQuad(newQuad->getId());
        // Switch to Quad properties and clear other panel selections (deliberate
        // cross-panel coordination via EditorState — see ADR-0034 / TD-02 contract).
        showQuadProperties(es);
    }

    ImGui::SameLine();

    // Delete button
    if (ImGui::Button("Delete") && quadManager.hasSelection()) {
        es.deleteSelectedQuads();
    }

    ImGui::SameLine();

    // Duplicate button
    // The duplicate carries the source's TextureBinding (mediaId), so it shows the
    // same media; the saved media path is copied so the .qmap keeps it too.
    if (ImGui::Button("Duplicate") && quadManager.getPrimarySelectedQuad()) {
        const Quad& source = *quadManager.getPrimarySelectedQuad();
        const UUID sourceId = source.getId();
        auto cmd = std::make_unique<DuplicateQuadCommand>(es.quadRef(source));
        const UUID dupId = cmd->getDuplicateId();
        es.commandHistory.execute(std::move(cmd));
        if (quadManager.getQuad(dupId)) {
            auto pathIt = quadTexturePaths.find(sourceId);
            if (pathIt != quadTexturePaths.end()) {
                quadTexturePaths[dupId] = pathIt->second;
            }
        }
    }

    // Group creation button (enabled when multiple quads selected)
    if (quadManager.getSelectionCount() >= 2) {
        ImGui::SameLine();
        if (ImGui::Button("Group")) {
            scene.createGroupFromSelection();
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
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Bring Forward (Ctrl+])");
        }
        ImGui::SameLine();

        ImGui::BeginDisabled(isLast);
        if (ImGui::SmallButton("v")) {
            quadManager.moveQuadUp(quad->getId());
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
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Locked");
        }
        ImGui::SameLine();

        // Quad name — inline editable when renaming this quad, else a selectable.
        const bool renamingThis = quadRenaming_ && quadRenameId_ == quad->getId();
        if (renamingThis) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##quadrename", quadNameBuf_, sizeof(quadNameBuf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                es.commandHistory.execute(
                    std::make_unique<RenameQuadCommand>(es.quadRef(*quad), quadNameBuf_));
                quadRenaming_ = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) quadRenaming_ = false;
        } else {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (isSelected) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        ImGui::TreeNodeEx(quad->getName().c_str(), flags);

        // Double-click the name to rename inline.
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            quadRenaming_ = true;
            quadRenameId_ = quad->getId();
            std::strncpy(quadNameBuf_, quad->getName().c_str(), sizeof(quadNameBuf_) - 1);
            quadNameBuf_[sizeof(quadNameBuf_) - 1] = '\0';
        }
        }

        // Row interaction (drag/drop/select) — skipped while inline-renaming so it
        // doesn't bind to the rename InputText.
        if (!renamingThis) {
        // Drag source for moving quads between groups / reordering layers
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            dragDropQuadId = quad->getId();
            ImGui::SetDragDropPayload("QUAD_DND", &dragDropQuadId, sizeof(UUID));
            ImGui::Text("Move: %s", quad->getName().c_str());
            ImGui::EndDragDropSource();
        }

        // Drop target on this row: media from the bin -> assign to THIS quad (ADR-042
        // P4); another quad -> reorder layer order to this quad's slot (ADR-042 P5).
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* mp = ImGui::AcceptDragDropPayload("MEDIA_ITEM_ID")) {
                UUID mid = *(const UUID*)mp->Data;
                Quad* mutableQuad = quadManager.getQuad(quad->getId());
                MediaItem* item = ctx.mediaLibrary ? ctx.mediaLibrary->getItem(mid) : nullptr;
                if (mutableQuad && item) {
                    assignMediaToQuad(es, ctx.sourceManager, *mutableQuad, *item);
                }
            }
            if (const ImGuiPayload* qp = ImGui::AcceptDragDropPayload("QUAD_DND")) {
                UUID draggedId = *(const UUID*)qp->Data;
                Quad* dragged = quadManager.getQuad(draggedId);
                if (dragged && draggedId != quad->getId()) {
                    // Step the dragged quad's z-order to the target's slot. (moveQuadDown
                    // raises z toward the front; moveQuadUp lowers it — see the ^/v buttons.)
                    const int target = quad->getZOrder();
                    int guard = static_cast<int>(quadManager.getQuadCount()) + 1;
                    while (dragged->getZOrder() < target && guard-- > 0)
                        quadManager.moveQuadDown(draggedId);
                    guard = static_cast<int>(quadManager.getQuadCount()) + 1;
                    while (dragged->getZOrder() > target && guard-- > 0)
                        quadManager.moveQuadUp(draggedId);
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::IsItemClicked()) {
            if (io.KeyCtrl) {
                quadManager.toggleSelection(quad->getId());
            } else if (io.KeyShift && quadManager.getPrimarySelectedId().has_value()) {
                quadManager.selectRange(*quadManager.getPrimarySelectedId(), quad->getId());
            } else {
                quadManager.selectQuad(quad->getId());
            }

            showQuadProperties(es);  // and clear the other panels' selections
        }
        }  // end if (!renamingThis)

        // Context menu for z-order and groups
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Rename")) {
                quadRenaming_ = true;
                quadRenameId_ = quad->getId();
                std::strncpy(quadNameBuf_, quad->getName().c_str(), sizeof(quadNameBuf_) - 1);
                quadNameBuf_[sizeof(quadNameBuf_) - 1] = '\0';
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Bring to Front", "Ctrl+Shift+]")) {
                quadManager.moveQuadToBottom(quad->getId());
            }
            if (ImGui::MenuItem("Bring Forward", "Ctrl+]")) {
                quadManager.moveQuadDown(quad->getId());
            }
            if (ImGui::MenuItem("Send Backward", "Ctrl+[")) {
                quadManager.moveQuadUp(quad->getId());
            }
            if (ImGui::MenuItem("Send to Back", "Ctrl+Shift+[")) {
                quadManager.moveQuadToTop(quad->getId());
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
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Toggle group visibility");
        }
        ImGui::SameLine();

        // Group lock checkbox
        bool groupLocked = group->isLocked();
        if (ImGui::Checkbox("##grpLocked", &groupLocked)) {
            scene.applyGroupLock(group->getId(), groupLocked);
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
                ImGui::CloseCurrentPopup();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Delete Group")) {
                scene.deleteGroup(group->getId());
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
            }
            ImGui::EndDragDropTarget();
        }
    }

    renderObjects(ctx);

    ImGui::End();
}

// The active scene's 3D objects (ADR-052): select, rename, show/hide and delete, each
// an undo step (TD-16).
void QuadListPanel::renderObjects(EditorContext& ctx) {
    EditorState& es = ctx.es;
    Scene& scene = es.activeScene();

    ImGui::Separator();
    ImGui::Text("Objects: %zu", scene.getObjects().size());
    if (scene.getObjects().empty()) {
        ImGui::TextDisabled("File > Import 3D Object (OBJ)... adds one");
    }

    UUID deleteId;
    for (const auto& objectPtr : scene.getObjects()) {
        MeshObject& object = *objectPtr;
        ImGui::PushID(object.getId().toString().c_str());

        bool visible = object.isVisible();
        if (ImGui::Checkbox("##objVisible", &visible)) {
            es.commandHistory.execute(makeSetObjectVisibilityCommand(es.objectRef(object), visible));
        }
        ImGui::SetItemTooltip("Visible");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) deleteId = object.getId();
        ImGui::SetItemTooltip("Delete object");
        ImGui::SameLine();

        auto beginRename = [&]() {
            objectRenaming_ = true;
            objectRenameId_ = object.getId();
            std::snprintf(objectNameBuf_, sizeof(objectNameBuf_), "%s", object.getName().c_str());
        };

        if (objectRenaming_ && objectRenameId_ == object.getId()) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##objrename", objectNameBuf_, sizeof(objectNameBuf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                if (object.getName() != objectNameBuf_) {
                    es.commandHistory.execute(makeRenameObjectCommand(es.objectRef(object), objectNameBuf_));
                }
                objectRenaming_ = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) objectRenaming_ = false;
        } else {
            const bool selected = es.currentSelectionType == EditorState::SelectionType::Object &&
                                  es.selectedObjectId == object.getId();
            const char* state = isMeshLoading(es, object)          ? " (loading...)"
                                : object.getMesh().empty()           ? " (mesh missing)"
                                                                     : "";
            const std::string label = object.getName() + state;
            if (ImGui::Selectable(label.c_str(), selected)) {
                es.selectedObjectId = object.getId();
                es.currentSelectionType = EditorState::SelectionType::Object;
                // A quad left selected behind the object's properties would still take
                // Delete and the arrow keys.
                scene.getQuadManager().deselectAll();
                es.selectedMediaId = UUID();
                es.selectedOutputId = UUID();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                beginRename();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Rename")) beginRename();
                if (ImGui::MenuItem("Delete")) deleteId = object.getId();
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
    }

    // Undoable like a quad delete, so no confirmation.
    if (!deleteId.isNull()) {
        es.commandHistory.execute(
            std::make_unique<DeleteObjectCommand>(ObjectRef{&es.quadResolver, scene.getId(), deleteId}));
        if (es.selectedObjectId == deleteId) es.selectedObjectId = UUID();
        if (objectRenameId_ == deleteId) objectRenaming_ = false;
    }
}

}  // namespace quadmapper
