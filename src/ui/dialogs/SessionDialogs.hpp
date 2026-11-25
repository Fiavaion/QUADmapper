#pragma once

#include "ui/panels/Panel.hpp"

namespace quadmapper {

// The session's modals (ADR-046): "Unsaved Changes" (Save / Don't Save / Cancel before
// New, Open or Exit) and "Recover Unsaved Work" (Restore / Discard). They read and
// answer through EditorState; the session acts on the answer at the next frame
// boundary. Also "Unlock the Show?", the perform lock's confirmation (ADR-051 §4).
// Drawn last, so they sit on top.
class SessionDialogs : public Panel {
public:
    static constexpr const char* ID = "Session Dialogs";

    const char* id() const override { return ID; }
    const char* menuLabel() const override { return nullptr; }
    PanelKind kind() const override { return PanelKind::Overlay; }
    bool editsShow() const override { return false; }  // Exit still confirms under the lock
    void drawContent(EditorContext& ctx) override;
};

}  // namespace quadmapper
