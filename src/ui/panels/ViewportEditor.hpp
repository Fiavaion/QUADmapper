#pragma once

#include "core/Types.hpp"

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
// quad editing is suspended and, when that projector shows the active scene, the canvas
// shows its output frame (objects from its projector, slices of the scene's canvas over
// them, TD-29) and the same mouse and keyboard model places, drags and nudges its
// calibration points, in projector pixels, instead.
// While a slice warp is edited (EditorState::sliceWarpOutputId, ADR-047) the canvas
// shows that output's frame and the same model moves the slice's warp corners; the
// selected output's slice input rectangles are outlined on the scene canvas otherwise.
//
// Does NOT inherit from Panel — panels draw ImGui windows;
// this draws via the Renderer and toggles GL depth state. It is stateless: all
// editing state (corner/mesh/snap selection + drag anchors) lives in EditorState
// and is reached through the per-frame EditorContext.
class ViewportEditor {
public:
    ViewportEditor() = default;

    // The mouse over the editor window: its position in window coordinates (GLFW's
    // logical pixels) and whether the left button is down.
    struct Pointer {
        Vec2 position{0.0f, 0.0f};
        bool leftDown = false;
        bool rightDown = false;  // a right click on a grid point opens its menu
    };

    // Call once per frame, right after ImGui::NewFrame(), from Application::render().
    // Reads the pointer from the editor window.
    void render(EditorContext& ctx);
    // The same frame with the pointer given (tests drive the canvas with it).
    void render(EditorContext& ctx, const Pointer& pointer);
};

}  // namespace quadmapper
