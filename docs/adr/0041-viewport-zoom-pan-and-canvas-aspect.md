# ADR-041 — Editor viewport: zoom/pan + canvas-aspect (portrait + landscape)

- **Status:** Accepted
- **Date:** 2026-06-19
- **Task:** QM-073
- **Supersedes / relates:** ADR-0034 (ViewportEditor extraction), ADR-039 (multi-scene output routing)

## Context

The on-canvas editor draws every quad's normalized `[0,1]` space **directly across the
whole GLFW window** — every vertex shader does `ndc = a_Position * 2 - 1; ndc.y = -ndc.y`.
Two problems fall out of that:

1. **No aspect correctness.** The `[0,1]` canvas is stretched to the *window's* aspect, not
   the projector's. A 16:9 frame is distorted in the editor, and there is no notion of "the
   projector frame" as a bounded rectangle. A **portrait** (9:16) projector is impossible to
   author against.
2. **Content hides behind the panels.** The GL canvas fills the entire window *behind* the
   ImGui dock panels (Quad List / Properties / Resources), so anything past the central
   region is occluded — the user literally cannot see the bottom/right of their own frame
   (reported with a screenshot).

## Decision

Introduce a per-session **view transform** (zoom + pan) and render the canvas **into the
dockspace central node** with the **projector's aspect ratio**.

- **Shader transform.** Add two uniforms — `u_ViewScale` (vec2) and `u_ViewOffset` (vec2) —
  to `quad.vert`, `outline.vert`, and `handle.vert`. The map becomes
  `view = a_Position * u_ViewScale + u_ViewOffset; ndc = view*2-1; ndc.y = -ndc.y`. Defaults
  `scale=(1,1)`, `offset=(0,0)` reproduce today's behavior **exactly**, so the per-output
  windows and preview FBOs (which never set the transform) are byte-for-byte unchanged.
- **Render target = central node.** `ViewportEditor` sets `glViewport` + `glScissor` to the
  dockspace **central node** rect (captured the previous frame via
  `DockBuilderGetCentralNode`), so `[0,1]` view-space *is* the visible region between panels.
  Scissor also clips drawing to that region — quads can no longer bleed under a panel. The
  viewport + scissor are restored to the full framebuffer at the end of the call.
- **Canvas aspect = the assigned output's effective resolution.** Derived from
  `OutputWindow::getEffectiveResolution()` (which already swaps W/H for 90°/270° rotation),
  so **portrait is automatic**: rotate the projector / set a tall resolution and the canvas
  follows. Falls back to 16:9 when the active scene has no assigned output. The canvas is
  letterboxed to this aspect inside the central node (`baseFit` margin), then scaled by zoom
  and translated by pan.
- **Hit-test inverts the same transform.** The success-metric warp path replaces
  `mouseNorm = cursor / windowSize` with `mouseNorm = (cursorInRegion - offset) / scale`.
  Render and hit-test read the *same* per-frame `(scale, offset)` from `EditorState`, so a
  handle is always under the cursor — the warp UX is preserved.
- **Controls.** A compact overlay (Fit / − / zoom% / +) plus scroll-wheel zoom (anchored on
  the cursor) and middle-drag pan, handled post-`NewFrame` in `Application` (writing
  `EditorState.canvasZoom`/`canvasPan` for the next frame — one-frame lag, invisible).

### Why not persist the view / why not a manual aspect toggle

- **Session-only view (no `.qmap` change).** Zoom/pan are editor view prefs, not project
  data. Keeping them out of the file avoids a format-version bump — a no-edit-without-approval
  Change Boundary — for zero user benefit. Canvas aspect is **derived**, never stored.
- **Auto aspect from the output** beats a manual toggle: it is correct by construction and
  needs no syncing with the real projector.

## Consequences

- New cross-cutting render state, but additive and identity-default — output/preview paths
  are provably unaffected.
- `mesh.vert`/`mesh.frag` remain dead (not loaded by `Renderer`); left untouched (out of
  scope). Noted so the transform asymmetry is intentional, not an oversight.
- Shader source is embedded at configure time (`ShaderSources.hpp` is generated), so editing
  a `.vert` requires re-running `cmake --preset debug` before the build picks it up.

## Acceptance criteria (the gate)

1. The **entire** projector frame is visible inside the central region at default zoom (Fit),
   with the correct aspect and a visible canvas border — nothing hidden behind a panel.
2. A **portrait** output (effective resolution H > W, e.g. via 90° rotation or a 1080×1920
   mode) produces a tall canvas, letterboxed, not stretched.
3. **Zoom** (buttons + wheel) and **pan** (middle-drag) work; wheel zoom stays anchored on
   the cursor; **Fit** restores the framed view.
4. The **success metric is intact**: select a vertex, Tab/Shift+Tab cycles, arrows nudge
   (Shift 10× / Alt 0.1×), corner drag lands under the cursor, and F11 sends the quad
   fullscreen to the projector — all still correct after the change.
5. `ctest` stays green; the app builds and runs.
