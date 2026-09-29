#pragma once

#include "editor/EditorState.hpp"
#include "ui/panels/Panel.hpp"

namespace quadmapper {

// Missing media (PRD §4.1.8, editor/MissingMedia.hpp): lists the files a loaded project
// could not open, with Locate and Search Folder (the session, ctx.services). A plain
// window, not a modal: the unsaved-changes and recovery modals can open over it, and
// closed it draws nothing, so it never holds the canvas keys. Opened by a load that
// found files missing (EditorState::requestPanelOpen) and by File > Relocate Missing
// Media...; it closes itself once nothing is missing.
class MissingMediaWindow : public Panel {
public:
    static constexpr const char* ID = EditorState::MISSING_MEDIA_PANEL_ID;

    MissingMediaWindow() { visible = false; }

    const char* id() const override { return ID; }
    const char* menuLabel() const override { return nullptr; }  // File > Relocate Missing Media...
    int windowFlags() const override;
    void tick(EditorContext& ctx) override;
    void beforeBegin(EditorContext& ctx) override;
    void drawContent(EditorContext& ctx) override;
};

}  // namespace quadmapper
