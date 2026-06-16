#pragma once

namespace quadmapper {

struct EditorContext;

// Renders and edits the on-canvas viewport: draws every quad to the main GLFW
// framebuffer via the OpenGL Renderer, then handles the corner-pin / mesh-edit
// warp interaction (mouse drag with vertex snapping, plus the keyboard
// Tab/Shift+Tab vertex cycle and arrow-key nudge that are the application's
// success-metric UX). This is the warp surface itself, not an ImGui widget.
//
// MUST be called BEFORE ImGui::NewFrame(): it owns the fullscreen GL viewport
// background and intentionally reads the mouse/keyboard ahead of any panel so a
// drag/nudge on a vertex is captured before the UI. The keyboard path is gated
// by !io.WantTextInput && !io.WantCaptureKeyboard so typing in a panel never
// nudges a vertex. (See ADR-0034 / the CLAUDE.md success metric.)
//
// Does NOT inherit from Panel — panels run post-NewFrame and draw ImGui windows;
// this draws via the Renderer and toggles GL depth state. It is stateless: all
// editing state (corner/mesh/snap selection + drag anchors) lives in EditorState
// and is reached through the per-frame EditorContext.
class ViewportEditor {
public:
    ViewportEditor() = default;

    // Call once per frame, before ImGui::NewFrame(), from Application::render().
    void render(EditorContext& ctx);
};

}  // namespace quadmapper
