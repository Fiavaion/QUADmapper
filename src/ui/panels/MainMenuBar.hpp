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

    // Call once per frame inside the ImGui frame, after the dockspace.
    void render(EditorContext& ctx);

private:
    ProjectCallback onNewProject_;
    ProjectCallback onSaveProject_;
    ProjectCallback onLoadProject_;
};

}  // namespace quadmapper
