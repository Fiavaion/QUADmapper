#pragma once

namespace quadmapper {

struct EditorContext;

// Edits and renders the on-canvas viewport: handles the corner-pin / mesh-edit
// warp interaction (mouse drag with vertex snapping, plus the keyboard
// Tab/Shift+Tab vertex cycle and arrow-key nudge that are the application's
// success-metric UX), then draws every quad to the main GLFW framebuffer via the
// OpenGL Renderer, so an edit shows in the frame that handled it (TD-21). This is
// the warp surface itself, not an ImGui widget.
//
// Call right after ImGui::NewFrame() and claimCanvasEditingKeys(), before any
// panel: it owns the fullscreen GL viewport background and reads the
// mouse/keyboard ahead of every panel, from the input that NewFrame() just took
// in. The keyboard path is gated by canvasOwnsEditingKeys()
// (input/VertexKeyboard.hpp) so typing in a panel never nudges a vertex, and
// Application claims Tab/arrows after NewFrame so a focused panel cannot swallow
// them. (See ADR-0034 / the CLAUDE.md success metric.)
// While a projector is being calibrated (EditorState::calibratingOutputId, ADR-052)
// quad editing is suspended and, when the canvas shows that projector, the same mouse
// and keyboard model places, drags and nudges its calibration points instead.
//
// Does NOT inherit from Panel — panels draw ImGui windows;
// this draws via the Renderer and toggles GL depth state. It is stateless: all
// editing state (corner/mesh/snap selection + drag anchors) lives in EditorState
// and is reached through the per-frame EditorContext.
class ViewportEditor {
public:
    ViewportEditor() = default;

    // Call once per frame, right after ImGui::NewFrame(), from Application::render().
    void render(EditorContext& ctx);
};

}  // namespace quadmapper
