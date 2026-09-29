#pragma once

namespace quadmapper {

struct EditorContext;

// Where the PanelHost draws a panel (ADR-051 §2).
enum class PanelKind {
    Window,   // its own dockable window: the host does Begin(id, &visible) / End
    Tab,      // a tab of the Resources window; always listed there
    Overlay,  // draws itself (its own windows, popups or the foreground draw list),
              // after every Window panel; the host only calls drawContent
};

// One editor panel (ADR-051 §2). Panels get everything through EditorContext, never
// through setters or callbacks, and own their visibility (`visible`); editor code asks
// for a window with EditorState::requestPanelOpen(id), which the host drains.
class Panel {
public:
    virtual ~Panel() = default;

    // The ImGui window name (Window), the tab label (Tab), and the dock and imgui.ini
    // name: never change it, or saved dock layouts lose the panel.
    virtual const char* id() const = 0;
    // The Window menu label; nullptr leaves the panel out of the menu.
    virtual const char* menuLabel() const { return id(); }
    virtual PanelKind kind() const { return PanelKind::Window; }
    // Drawn disabled under the perform lock (E7). Only show controls return false.
    virtual bool editsShow() const { return true; }

    // Once, when the editor's components exist (replaces lazy setters and callbacks).
    virtual void onAttach(EditorContext&) {}
    // After New, Open or Restore replaced the show (outside the ImGui frame).
    virtual void onProjectReplaced(EditorContext&) {}
    // The panel's body. For a Window panel the host has done Begin and does End.
    virtual void drawContent(EditorContext&) = 0;

    // ---- Hooks beyond ADR-051 §2 (as built in E5) --------------------------------
    // Every frame at the panel's place in the draw order, shown or not: work that must
    // not wait for the window (a finished file dialog, following the selection).
    virtual void tick(EditorContext&) {}
    // False: not drawn this frame (Simple mode hides the Advanced tabs).
    virtual bool available(const EditorContext&) const { return true; }
    // Window panels: ImGuiWindowFlags for Begin, and SetNextWindow* calls made just
    // before it (only when the window is drawn).
    virtual int windowFlags() const { return 0; }
    virtual void beforeBegin(EditorContext&) {}

    bool visible = true;
};

}  // namespace quadmapper
