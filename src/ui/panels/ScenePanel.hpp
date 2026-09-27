#pragma once

#include "core/UUID.hpp"

namespace quadmapper {

class Scene;
struct EditorContext;

// Scenes panel (ADR-039 P3). Makes the per-output scene routing user-operable:
// create / duplicate / delete / rename / reorder / switch the ACTIVE scene, and
// assign each scene to an output window. Scenes 1-9 show their take hotkey. The viewport, Quad List and Inspector all edit the active
// scene (EditorState::activeScene()), so switching here makes them follow.
//
// Rendered as a tab inside the Resources panel via renderContent(ctx); it owns
// no scene state itself — that lives in EditorState, reached through the
// per-frame EditorContext. Only transient rename UI state is local.
class ScenePanel {
public:
    ScenePanel() = default;

    // Render the panel body (no window wrapper) for embedding in a tab.
    void renderContent(EditorContext& ctx);

private:
    void renderActionButtons(EditorContext& ctx);
    void renderCanvasControls(EditorContext& ctx);
    void renderSceneList(EditorContext& ctx);
    void beginRename(const Scene& scene);

    // Tracked by id: duplicate and reorder move scenes to other list positions.
    UUID renamingId_;  // null = not renaming
    char renameBuffer_[128] = "";

    // The active scene's canvas size as typed; applied when a field is left (ADR-047).
    UUID canvasSceneId_;
    int canvasEdit_[2] = {0, 0};
    bool canvasEditing_ = false;  // a canvas field was being typed in last frame
};

}  // namespace quadmapper
