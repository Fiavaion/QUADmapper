#pragma once

namespace quadmapper {

struct EditorContext;

// Live per-output preview (ADR-039 P4). Shows a small thumbnail of what each
// output projects: the output's assigned scene rendered to an offscreen FBO by
// OutputManager::updatePreviews() once per frame. Display-only — it just samples
// the textures the OutputManager already produced. Rendered as a tab inside the
// Resources panel via renderContent(ctx).
class OutputPreviewPanel {
public:
    OutputPreviewPanel() = default;

    void renderContent(EditorContext& ctx);
};

}  // namespace quadmapper
