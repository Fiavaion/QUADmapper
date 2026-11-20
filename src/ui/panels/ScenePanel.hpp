#pragma once

#include "Panel.hpp"
#include "core/UUID.hpp"
#include "ui/ConfirmPopup.hpp"

namespace quadmapper {

class Scene;
struct EditorContext;

// Scenes panel (ADR-039 P3). Makes the per-output scene routing user-operable:
// create / duplicate / delete / rename / reorder / switch the ACTIVE scene, and
// assign each scene to an output window. Scenes 1-9 show their take hotkey. The viewport, Quad List and Inspector all edit the active
// scene (EditorState::activeScene()), so switching here makes them follow.
//
// The Scenes tab of the Resources window, in both UI modes (one scene per projector,
// switched between while editing; the canvas size only in Advanced); it owns no scene state
// itself — that lives in EditorState, reached through the per-frame EditorContext.
// Only transient rename UI state is local.
class ScenePanel : public Panel {
public:
    static constexpr const char* ID = "Scenes";

    ScenePanel() = default;

    const char* id() const override { return ID; }
    PanelKind kind() const override { return PanelKind::Tab; }
    void drawContent(EditorContext& ctx) override;

private:
    void renderActionButtons(EditorContext& ctx);
    void renderCanvasControls(EditorContext& ctx);
    void renderSceneList(EditorContext& ctx);
    void beginRename(const Scene& scene);
    // Ask before deleting a scene; the delete runs when the modal is confirmed.
    void requestDelete(EditorContext& ctx, const UUID& id);
    void renderDeleteConfirm(EditorContext& ctx);

    // Tracked by id: duplicate and reorder move scenes to other list positions.
    UUID renamingId_;  // null = not renaming
    char renameBuffer_[128] = "";

    ConfirmPopup deleteConfirm_{"scenedelete"};
    UUID pendingDeleteId_;  // the scene the open confirmation is about

    // The active scene's canvas size as typed; applied when a field is left (ADR-047).
    UUID canvasSceneId_;
    int canvasEdit_[2] = {0, 0};
    bool canvasEditing_ = false;  // a canvas field was being typed in last frame
};

}  // namespace quadmapper
