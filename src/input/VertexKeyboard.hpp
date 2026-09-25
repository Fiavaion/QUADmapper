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

// True when the canvas should act on Tab / arrow keys: a quad is selected, nothing
// is being typed or edited in a widget, and no popup or modal is open.
inline bool canvasOwnsEditingKeys(bool quadSelected) {
    const ImGuiIO& io = ImGui::GetIO();
    return quadSelected && !io.WantTextInput && !io.WantCaptureKeyboard &&
           !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

// Call once per frame right after ImGui::NewFrame(). While the canvas owns the
// editing keys, it claims Tab and the arrows so a focused panel's tabbing and
// keyboard navigation (which only act on unowned keys) leave them alone; the vertex
// then moves no matter which panel was clicked last ("+ Add Quad", the media bin, the
// Inspector). Ownership set now applies from the next NewFrame, when the next key
// press arrives, and lapses on its own once the claim stops.
inline void claimCanvasEditingKeys(bool quadSelected) {
    if (!canvasOwnsEditingKeys(quadSelected)) return;
    const ImGuiID owner = ImHashStr("QUADmapper.CanvasEditingKeys");
    for (ImGuiKey key : {ImGuiKey_Tab, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
                         ImGuiKey_LeftArrow, ImGuiKey_RightArrow}) {
        ImGui::SetKeyOwner(key, owner);
    }
}

}  // namespace quadmapper
