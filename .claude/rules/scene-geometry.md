---
paths:
  - "src/scene/**"
  - "src/input/**"
---
# Scene, Geometry & Input Rules (loaded when touching scene/input)

- **The vertex-editing UX is the product's core success metric** — see
  [Application.cpp:674-808](../../src/app/Application.cpp#L674-L808). Behavior to preserve:
  `Tab`/`Shift+Tab` cycles verts, arrow keys nudge the selected vert, **`Shift` = 10×**,
  `Alt` = 0.1× fine, vertex snapping to neighbouring quads. Works in CORNERS_ONLY (4 corners)
  and mesh-edit modes. Changing nudge amounts/keybinds needs explicit sign-off.
- **Corner pin (CORNERS_ONLY) is the default mode**; mesh edit is opt-in via checkbox
  (ADR-027). Mesh offsets are stored relative to bilinear positions, not absolute.
- Geometry changes must set `m_project->setModified(true)` so the dirty flag / autosave work.
- Mesh subdivision resolution is bounded (`MAX_MESH_RESOLUTION`); respect the cap.
- Keyboard handling must guard on `!io.WantTextInput && !io.WantCaptureKeyboard` so typing in
  ImGui fields never moves vertices.
