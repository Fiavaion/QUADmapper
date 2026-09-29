#pragma once

#include "ui/panels/Panel.hpp"

#include <string>
#include <vector>

namespace quadmapper {

struct EditorContext;
struct EditorState;

// Draws the editor's panels (ADR-051 §2): each Window panel in its own window, the Tab
// panels inside the Resources window (drawTabs), then the Overlay panels; generates the
// Window menu from them; and opens the windows editor code asked for. Holds the panels
// by pointer, in draw order; their owner (EditorUi, or Application for Preferences)
// keeps them alive while the host is used.
class PanelHost {
public:
    // Ids must be unique. The panel's `visible` at this point is its default, which
    // Window > Reset Layout restores for the panels shown by default.
    void add(Panel& panel);

    void attach(EditorContext& ctx);           // onAttach, each panel
    void projectReplaced(EditorContext& ctx);  // onProjectReplaced, each panel

    // Opens the windows asked for through EditorState::requestPanelOpen, then ticks
    // and draws the Window panels in order. Tab panels are ticked here and drawn by
    // whoever calls drawTabs (the Resources window).
    void drawWindows(EditorContext& ctx);
    // One tab item per available Tab panel, inside the caller's BeginTabBar.
    void drawTabs(EditorContext& ctx);
    // Ticks and draws the Overlay panels in order.
    void drawOverlays(EditorContext& ctx);

    // The Window menu's items (call between BeginMenu/EndMenu): a toggle per listed
    // Window panel, then Reset Layout.
    void drawWindowMenuItems();

    Panel* find(const std::string& id) const;
    bool isVisible(const std::string& id) const;
    const std::vector<Panel*>& panels() const { return panels_; }

private:
    void drainOpenRequests(EditorState& es);
    // The one place the perform lock (ADR-051 §4) disables show-editing panels: on,
    // and the panel's editsShow().
    bool drawDisabled(const Panel& panel, const EditorContext& ctx) const;

    std::vector<Panel*> panels_;
    std::vector<bool> defaultVisible_;
};

}  // namespace quadmapper
