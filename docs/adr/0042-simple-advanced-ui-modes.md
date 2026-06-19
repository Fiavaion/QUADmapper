# ADR-042 — Simple / Advanced UI modes

- **Status:** Accepted
- **Date:** 2026-06-19
- **Task:** QM-075
- **Relates:** ADR-0034 (ViewportEditor), ADR-040 (shared sources + crop), ADR-041 (viewport zoom/pan)

## Context

The UI exposed every advanced control at once — the Inspector alone has ~10 sections — which
is a steep barrier for a non-technical user or student setting up a basic mapping. The backend
is solid and unchanged; the problem is purely UI/UX surface area.

## Decision

Add a per-session **UI mode** (`EditorState::UiMode { Simple, Advanced }`) with a prominent
toolbar toggle in the main menu bar. **Advanced** is the full existing UI, unchanged.
**Simple** is a curated subset for the common single-projector workflow. Default = **Simple**,
persisted in `preferences.json` (`general.uiMode`, additive — no `.qmap` change).

Simple mode:
- **Inspector**: only Flip U/V, Rotation (+0/90/180/270), Crop (L/R/T/B), and **Reset Quad**
  (full reset of warp/flip/rotate/crop; keeps the assigned media). On-canvas corner warping
  (drag / Tab / arrows) stays available in both modes.
- **Video Player**: only **Load Video...** (browses, assigns to the selected quad), Play/Pause,
  Loop, Speed.
- **Resources**: only the **Media** bin tab. Project/Scenes/Preview/Outputs are Advanced-only;
  output is auto (the app auto-creates a projector on the 2nd monitor) plus a toolbar
  **Send to Projector (F11)** button.
- **Toolbar**: also surfaces **Apply Test Grid** (on the selected quad).

Cross-cutting helpers added (benefit both modes):
- **`assignMediaToQuad` / `bindSourceToQuad`** (`src/app/MediaAssign.hpp`) — one place that binds
  a media item/source to a quad (mediaId + path + legacy-texture cleanup + dirty flag). Replaces
  three duplicated copies and fixes a latent bug where the Inspector created an *image* source
  for dropped *videos*.
- **Media bin**: `Load Files...` and `Load Folder...` (Win32 `SHBrowseForFolder` →
  `MediaLibrary::scanFolder`). Explorer→window file drop was already wired.
- **Drag media onto a quad**: a canvas drop target via `ImGui::BeginDragDropTargetCustom`
  (active only during a drag, so it never disturbs the warp clicks) + `pickTopQuadAt`
  (topmost quad via `QuadGeometry::inverseBilinear`); also droppable on a Quad List row.
- **Quad List**: drag a quad onto another row to reorder its layer (z-order).

## Why these choices

- **Mode as a thin flag + per-panel gating** (not a parallel UI tree) keeps one code path; the
  Advanced branch is the untouched original, so there is no risk of drift or regression.
- **Default Simple, persisted** serves the target user; the technical user flips once.
- **Auto single-projector + F11 button** covers the common case without a multi-output UI;
  multi-output stays Advanced.
- **Reuse over rebuild**: scanFolder, the GLFW drop callback, the media drag-source, the test
  grid, inverseBilinear, and the preferences store already existed.

## Consequences

- One additive preferences field; no `.qmap` change; backend untouched.
- The crop control is shared by both modes via `InspectorPanel::renderCropControls` (single source).
- `mesh.vert` and other Advanced-only controls are simply not shown in Simple mode (not removed).

## Acceptance criteria

1. Starts in Simple; toolbar toggle flips modes; choice survives restart.
2. Apply Test Grid works on the selected quad.
3. Media bin Load Files / Load Folder import; Explorer drag-in still works.
4. Drag a clip from the bin onto a quad (canvas) assigns to that quad; Quad List row drop works.
5. Load Video (Simple) loads + assigns to the selected quad; Loop + Speed work.
6. Simple Inspector shows only Flip U/V, Rotate, Crop, Reset Quad; Reset Quad returns the quad to
   a clean full-frame rectangle (media kept).
7. Quad List drag reorders layer order.
8. Send to Projector (F11) works; success-metric warp intact in BOTH modes.
9. Advanced mode unchanged; `ctest` green.
