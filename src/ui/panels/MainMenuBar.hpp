#pragma once

#include <functional>

namespace quadmapper {

struct EditorContext;

// Renders the application main menu bar (File / Edit / Window).
// Does NOT inherit from Panel — it is always shown and manages its own
// ImGui::BeginMainMenuBar / EndMainMenuBar lifetime.
// Project actions (New / Save / Load) are delegated to the three callbacks
// because those methods live on Application. Callers set them once at init time.
// All other items (Edit / Window) operate directly on ctx.es fields.
class MainMenuBar {
public:
    using ProjectCallback = std::function<void()>;

    void setOnNewProject(ProjectCallback cb)  { onNewProject_  = std::move(cb); }
    void setOnSaveProject(ProjectCallback cb) { onSaveProject_ = std::move(cb); }
    void setOnLoadProject(ProjectCallback cb) { onLoadProject_ = std::move(cb); }

    // AI pre-show assist (ADR-035 / QM-056). The menu is always shown; its items
    // enable/disable purely from aiActiveQuery_, so the optional sidecar's absence
    // degrades cleanly. The query reads cached client state (no per-frame I/O); the
    // two actions launch the sidecar and request a surface. All optional.
    using BoolQuery = std::function<bool()>;
    void setAiActiveQuery(BoolQuery cb)        { aiActiveQuery_   = std::move(cb); }
    void setOnEnableAi(ProjectCallback cb)     { onEnableAi_      = std::move(cb); }
    void setOnDetectSurface(ProjectCallback cb){ onDetectSurface_ = std::move(cb); }

    // One-Flash calibration flow (QM-059): emit a projector pattern, capture a
    // photo, clear the pattern.
    void setOnEmitPattern(ProjectCallback cb)  { onEmitPattern_  = std::move(cb); }
    void setOnCapturePhoto(ProjectCallback cb) { onCapturePhoto_ = std::move(cb); }
    void setOnClearPattern(ProjectCallback cb) { onClearPattern_ = std::move(cb); }

    // Call once per frame inside the ImGui frame, after the dockspace.
    void render(EditorContext& ctx);

private:
    ProjectCallback onNewProject_;
    ProjectCallback onSaveProject_;
    ProjectCallback onLoadProject_;

    BoolQuery       aiActiveQuery_;
    ProjectCallback onEnableAi_;
    ProjectCallback onDetectSurface_;
    ProjectCallback onEmitPattern_;
    ProjectCallback onCapturePhoto_;
    ProjectCallback onClearPattern_;
};

}  // namespace quadmapper
