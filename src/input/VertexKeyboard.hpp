#pragma once

// Keyboard vertex warping, the success-metric UX (CLAUDE.md): Tab / Shift+Tab cycles
// the selected vertex, the arrow keys nudge it, Shift = 10x, Alt = 0.1x. The maths
// is shared by the corner-pin and mesh-edit paths in ViewportEditor.

#include "core/Types.hpp"

#include <imgui.h>
#include <imgui_internal.h>  // SetKeyOwner, ImHashStr

namespace quadmapper {

// One arrow-key nudge in normalized canvas units.
constexpr float NUDGE_STEP = 0.001f;

inline float nudgeStep(bool shift, bool alt) {
    float step = NUDGE_STEP;
    if (shift) step *= 10.0f;
    if (alt) step *= 0.1f;
    return step;
}

// Vertex selected after Tab (or Shift+Tab when `backwards`). From no selection (-1)
// or a stale index, Tab selects the first vertex and Shift+Tab the last.
inline int cycleVertex(int current, int count, bool backwards) {
    if (count <= 0) return -1;
    if (backwards) return (current <= 0 || current >= count) ? count - 1 : current - 1;
    return (current < 0 || current >= count - 1) ? 0 : current + 1;
}

// Canvas-space offset for the arrow keys pressed this frame (y grows downwards).
inline Vec2 nudgeDelta(bool up, bool down, bool left, bool right, float step) {
    return Vec2((right ? step : 0.0f) - (left ? step : 0.0f),
                (down ? step : 0.0f) - (up ? step : 0.0f));
}

// In mesh edit, H (Shift+H backwards) cycles the selected vertex's grid-line handles
// (ADR-049 §3); the arrows then nudge the handle with the same steps. A bare H is no
// other key's: the editor uses Tab, the arrows, Delete, 1-9 / numpad 1-9 (scene
// take), F11, Esc, and Ctrl or Alt chords.
constexpr ImGuiKey GRID_HANDLE_KEY = ImGuiKey_H;

// Handle selected after H (or Shift+H when `backwards`), given `available` with one
// bit per handle (QuadGeometry::availableHandles, 4 handles). It steps from the
// vertex itself (-1) through each available handle and back to the vertex.
inline int cycleHandle(int current, unsigned available, bool backwards) {
    constexpr int HANDLES = 4;
    int h = (current >= -1 && current < HANDLES) ? current : -1;
    for (int step = 0; step <= HANDLES; ++step) {
        if (backwards) h = (h < 0) ? HANDLES - 1 : h - 1;
        else           h = (h + 1 >= HANDLES) ? -1 : h + 1;
        if (h < 0 || ((available >> h) & 1u)) return h;
    }
    return -1;
}

// The canvas editing keys pressed this frame: arrows auto-repeat, Tab and H do not.
struct CanvasKeys {
    bool tab = false;
    bool handle = false;
    bool up = false, down = false, left = false, right = false;
    bool shift = false, alt = false;

    bool arrow() const { return up || down || left || right; }
    Vec2 nudge() const { return nudgeDelta(up, down, left, right, nudgeStep(shift, alt)); }
};

// Read CanvasKeys from ImGui. Call only while canvasOwnsEditingKeys() holds.
inline CanvasKeys readCanvasKeys() {
    const ImGuiIO& io = ImGui::GetIO();
    CanvasKeys keys;
    keys.tab = ImGui::IsKeyPressed(ImGuiKey_Tab, false);
    keys.handle = !io.KeyCtrl && !io.KeyAlt && !io.KeySuper &&
                  ImGui::IsKeyPressed(GRID_HANDLE_KEY, false);
    keys.up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
    keys.down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
    keys.left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
    keys.right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
    keys.shift = io.KeyShift;
    keys.alt = io.KeyAlt;
    return keys;
}

// True when the canvas should act on Tab / arrow keys: a quad is selected, nothing
// is being typed or edited in a widget, and no popup or modal is open.
inline bool canvasOwnsEditingKeys(bool quadSelected) {
    const ImGuiIO& io = ImGui::GetIO();
    return quadSelected && !io.WantTextInput && !io.WantCaptureKeyboard &&
           !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

// Call once per frame right after ImGui::NewFrame(). While the canvas owns the
// editing keys, it claims Tab, H and the arrows so a focused panel's tabbing and
// keyboard navigation (which only act on unowned keys) leave them alone; the vertex
// then moves no matter which panel was clicked last ("+ Add Quad", the media bin, the
// Inspector). Ownership set now applies from the next NewFrame, when the next key
// press arrives, and lapses on its own once the claim stops.
inline void claimCanvasEditingKeys(bool quadSelected) {
    if (!canvasOwnsEditingKeys(quadSelected)) return;
    const ImGuiID owner = ImHashStr("QUADmapper.CanvasEditingKeys");
    for (ImGuiKey key : {ImGuiKey_Tab, GRID_HANDLE_KEY, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
                         ImGuiKey_LeftArrow, ImGuiKey_RightArrow}) {
        ImGui::SetKeyOwner(key, owner);
    }
}

}  // namespace quadmapper
