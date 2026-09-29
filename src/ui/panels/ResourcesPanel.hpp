#pragma once

#include "Panel.hpp"

namespace quadmapper {

class PanelHost;

// The "Resources" dockable window: a tab bar holding the host's Tab panels (Project,
// Media, Scenes, Preview, Outputs). Simple mode (ADR-042) shows only the Media bin.
class ResourcesPanel : public Panel {
public:
    static constexpr const char* ID = "Resources";

    explicit ResourcesPanel(PanelHost& host) : host_(host) {}

    const char* id() const override { return ID; }
    // Only the tab bar: each tab is drawn disabled or not by its own editsShow(), so
    // the Preview stays reachable under the perform lock.
    bool editsShow() const override { return false; }
    void drawContent(EditorContext& ctx) override;

private:
    PanelHost& host_;
};

// The Project tab (Advanced mode): the project's name and unsaved state, its path with
// Save and Load, and the last status message.
class ProjectTab : public Panel {
public:
    static constexpr const char* ID = "Project";

    const char* id() const override { return ID; }
    PanelKind kind() const override { return PanelKind::Tab; }
    bool available(const EditorContext& ctx) const override;
    void drawContent(EditorContext& ctx) override;
};

}  // namespace quadmapper
