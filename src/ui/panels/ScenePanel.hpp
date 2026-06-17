#pragma once

namespace quadmapper {

struct EditorContext;

// Scenes panel (ADR-039 P3). Makes the per-output scene routing user-operable:
// create / delete / rename / switch the ACTIVE scene, and assign each scene to
// an output window. The viewport, Quad List and Inspector all edit the active
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
    void renderSceneList(EditorContext& ctx);

    bool isRenaming_ = false;
    int renamingIndex_ = -1;
    char renameBuffer_[128] = "";
};

}  // namespace quadmapper
