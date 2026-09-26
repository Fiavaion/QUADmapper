#pragma once

namespace quadmapper {

struct EditorContext;

// Live per-output preview (ADR-039 P4). Shows a small thumbnail of what each
// output projects: the frame OutputManager::renderOutputFrames() drew for that
// output this frame, the same texture its projector window shows (ADR-054).
// Display-only: it just samples the textures the OutputManager already produced.
// Rendered as a tab inside the Resources panel via renderContent(ctx).
class OutputPreviewPanel {
public:
    OutputPreviewPanel() = default;

    void renderContent(EditorContext& ctx);
};

}  // namespace quadmapper
