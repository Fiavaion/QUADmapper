#pragma once

#include "Panel.hpp"

namespace quadmapper {

struct EditorContext;

// Live per-output preview (ADR-039 P4). Shows a small thumbnail of what each
// output projects: the frame OutputManager::renderOutputFrames() drew for that
// output this frame, the same texture its projector window shows (ADR-054).
// Display-only: it just samples the textures the OutputManager already produced.
// The Preview tab of the Resources window (Advanced mode).
class OutputPreviewPanel : public Panel {
public:
    static constexpr const char* ID = "Preview";

    const char* id() const override { return ID; }
    PanelKind kind() const override { return PanelKind::Tab; }
    bool available(const EditorContext& ctx) const override;
    void drawContent(EditorContext& ctx) override;
};

}  // namespace quadmapper
