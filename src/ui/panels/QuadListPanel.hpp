#pragma once

#include "Panel.hpp"
#include "core/UUID.hpp"

namespace quadmapper {

struct EditorContext;

// The "Quad List" dockable window (add/delete/duplicate quads, z-order buttons,
// visibility/lock toggles, group management with drag-drop, and inline group rename),
// with the active scene's 3D objects below.
class QuadListPanel : public Panel {
public:
    static constexpr const char* ID = "Quad List";

    QuadListPanel() = default;

    const char* id() const override { return ID; }
    void drawContent(EditorContext& ctx) override;

private:
    // The "Objects" section below the quads (3D objects, ADR-052).
    void renderObjects(EditorContext& ctx);

    // Inline rename buffer for group context-menu (replaces function-static).
    char groupNameBuf_[128] = "";

    // Inline quad rename (double-click a quad name, or context-menu > Rename).
    bool quadRenaming_ = false;
    UUID quadRenameId_;
    char quadNameBuf_[128] = "";

    // Inline object rename (double-click, or context-menu > Rename).
    bool objectRenaming_ = false;
    UUID objectRenameId_;
    char objectNameBuf_[128] = "";
};

}  // namespace quadmapper
