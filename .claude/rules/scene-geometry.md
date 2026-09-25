---
paths:
  - "src/scene/**"
  - "src/input/**"
---
# Scene, Geometry & Input Rules (loaded when touching scene/input)

- **The vertex-editing UX is the product's core success metric** — see
  [ViewportEditor.cpp](../../src/ui/panels/ViewportEditor.cpp) and
  [VertexKeyboard.hpp](../../src/input/VertexKeyboard.hpp). Behavior to preserve:
  `Tab`/`Shift+Tab` cycles verts, arrow keys nudge the selected vert, **`Shift` = 10×**,
  `Alt` = 0.1× fine, vertex snapping to neighbouring quads. Works in CORNERS_ONLY (4 corners)
  and mesh-edit modes. Changing nudge amounts/keybinds needs explicit sign-off.
- **Corner pin (CORNERS_ONLY) is the default mode**; mesh edit is opt-in via checkbox
  (ADR-027). Mesh offsets are stored relative to bilinear positions, not absolute.
- Geometry changes must set `m_project->setModified(true)` so the dirty flag / autosave work.
- Mesh subdivision resolution is bounded (`MAX_MESH_RESOLUTION`); respect the cap.
- Canvas keyboard handling must go through `canvasOwnsEditingKeys()` (no text input, no
  widget capturing the keyboard, no popup) so typing in ImGui fields never moves vertices;
  `claimCanvasEditingKeys()` after `NewFrame()` keeps a focused panel from swallowing Tab/arrows.
- Canvas clicks and drags start only on a fresh press over the canvas (`!io.WantCaptureMouse`),
  never on a held button crossing a handle.
