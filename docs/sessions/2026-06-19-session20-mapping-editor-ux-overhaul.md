# Session 20 — 2026-06-19 — Mapping-editor UX overhaul

**Branch:** `dev` (committed HEAD `1554216`). **⚠️ Entire session UNCOMMITTED** — 34 working-tree
changes (29 modified, 5 new). **Build:** green. **Tests:** 633/633 ctest. App launches.

This was a hands-on session: the user set up a real projector + surface and drove feature work by
testing live. Six feature areas landed. **Nothing is committed** — the first job next session is to
commit it in logical chunks (see `notes/CURRENT_STATUS.md` for per-chunk file lists).

## Features (all built + ctest-green; interactive bits need USER visual verification)

### QM-073 — Viewport zoom/pan + aspect canvas (ADR-041)
- Editor draws into the **dockspace central node** (between panels) with a view transform.
- `u_ViewScale` + `u_ViewOffset` added to `quad.vert`, `outline.vert`, `handle.vert` (identity
  default → per-output windows + preview FBOs render byte-identically).
- Canvas letterboxed to the active scene's **assigned output effective resolution** (portrait
  auto via 90°/270° swap); 16:9 fallback. Fit button, wheel-zoom (cursor-anchored), middle-drag pan.
- **Success-metric preserved:** the warp hit-test inverts the same `(scale, offset)`.
- Fixed `CropRenderTest` (drives the shader directly → must set the new uniforms or the quad
  collapses to a point).

### QM-074 — Native video picker + crop insets
- "Load Video…" opens a Win32 file dialog (typed-path "Use Path" kept as fallback).
- Crop UI reworked from X/Y/W/H to **Crop Left/Right/Top/Bottom %** (storage stays `{x,y,w,h}`);
  shared `renderCropControls`; two-face split presets ("Left ½"/"Right ½" align seamlessly).

### QM-075 — Simple/Advanced UI modes (ADR-042)
- `EditorState::UiMode` (persisted in `preferences.json`, default **Simple**). Toolbar toggle.
- Simple = curated Inspector (Flip/Rotate/Crop/Reset + bezier), Video Player (Load/Play/Loop/Speed),
  Media-only Resources, toolbar Apply-Test-Grid + Send-to-Projector(F11).
- Video **auto-plays on assign** (a paused video uploads no frames → black quad otherwise).
- **Drag a clip from the bin onto a canvas quad** — manual `GetDragDropPayload()` + `IsMouseReleased()`
  over the region (a custom ImGui drop target can't register over a `PassthruCentralNode`).
- **Click a quad to select it** (`QuadManager::quadAtPoint` via `inverseBilinear`).
- Centralized `assignMediaToQuad`/`bindSourceToQuad` (`src/app/MediaAssign.hpp`) — fixed a latent
  bug: dropped videos were created as image sources.
- Scenes "New" seeds a starter quad (empty scene read as "broken").

### QM-076 — Media-bin thumbnails + hover-to-play
- Video **first-frame** thumbnails: one-shot `VideoDecoder.open` + `decodeNextFrame` → RGBA →
  cached RAII `Texture` (no persistent decoder). Procedural sources draw their live GPU texture.
- **Hover-to-play**: one dedicated `createVideoSource` preview decoder (separate from any quad's
  assigned video), switched/torn-down as the hovered item changes.

### QM-077 — Undo/Redo
- Reused the dormant `CommandHistory` + command classes **as-is** (raw `Quad*`).
- `EditorState` owns the stack; Ctrl+Z/Y + Edit ▸ Undo/Redo.
- Routes: corner drag (release) + arrow-nudge, mesh drag + nudge, rename (Quad List + Inspector),
  and grid edits (Add/Del Col/Row, Even-Grid, click-insert) via a `GridUndo.hpp` snapshot command.
- **Crash-safe** via a guard in `Application::render` that **clears history on quad delete / scene
  switch** (so no command outlives its quad). Undoable create/delete deferred (needs `Quad::setId`
  + UUID-based commands).

### QM-078 — Report Bug + smooth distortion + bezier corners (ADR-043)
- Toolbar **Report Bug** → `github.com/Fiavaion/Quadmapper/issues/new?template=bug_report.md`.
- `QuadGeometry::surfacePoint(u,v)` is the single render sampler; `CORNERS_ONLY` quads tessellate
  **32×32** → the diagonal "fold" on extreme corner-pins is gone.
- **Bezier edges on the 4 corners**: 8 control-point offsets from the 1/3 & 2/3 points (zero =
  straight), blended as a **Coons patch** in `surfacePoint`. Amber drag handles
  (`Renderer::renderBezierHandles`), toggle + "Reset Curves" in **both** Inspectors, additive
  `.qmap` persistence, undoable per bend.

## Deferred / follow-ups
- **QM-079 — bezier on the grid division lines** (the user wants this next). Corner bezier is done;
  curving interior divisions is a larger surface-model change.
- **Undoable create/delete** (history currently clears on those).
- Revert `preferences.json` `defaultMediaFolder` (= `D:\testing Videos`, set for verification).

## Verification status
- Built + 633/633 ctest each stage. App launches clean.
- **Visual/interactive features NOT verified by the assistant** — desktop-sharing kept VS Code
  foreground, so reliable synthetic UI driving wasn't possible. User to verify: smooth distortion,
  bezier handles, hover-to-play, undo/redo, Scenes-New, canvas drag-drop, click-select.

## Key gotcha
The running app **locks `QUADmapper.exe`** — it must be closed before the link step (compile
succeeds, only `LNK1168` fails). This recurred all session.
