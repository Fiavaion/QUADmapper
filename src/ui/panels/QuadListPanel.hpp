#pragma once

#include "core/UUID.hpp"

namespace quadmapper {

struct EditorContext;

// Renders the "Quad List" dockable window (add/delete/duplicate quads, z-order
// buttons, visibility/lock toggles, group management with drag-drop, and inline
// group rename). Visibility is controlled by ctx.es.showQuadList; when false,
// render() returns immediately without opening the window.
//
// Does NOT inherit from Panel — the Panel::render() signature takes no arguments
// and this panel requires a per-frame EditorContext (same approach as MainMenuBar).
// Ownership of per-frame group-rename buffer is a member to avoid function-static state.
class QuadListPanel {
public:
    QuadListPanel() = default;

    // Call once per frame inside the ImGui frame, after the dockspace.
    void render(EditorContext& ctx);

private:
    // Inline rename buffer for group context-menu (replaces function-static).
    char groupNameBuf_[128] = "";

    // Inline quad rename (double-click a quad name, or context-menu > Rename).
    bool quadRenaming_ = false;
    UUID quadRenameId_;
    char quadNameBuf_[128] = "";
};

}  // namespace quadmapper
