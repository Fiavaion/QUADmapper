# Current Status

**Last updated:** 2026-06-19 (Session 20)
**Phase / focus:** **Mapping-editor UX overhaul** — viewport zoom/pan, Simple/Advanced UI,
media-bin thumbnails, undo/redo, smooth distortion, bezier corner edges. ADR-040 (one video →
many cropped quads) is COMPLETE (P1–P5, from Session 19). AI wedge (Phase 9) stays
software-complete, gated on 🚩 QM-061 (hardware).
**Branch:** `dev` (committed HEAD `1554216`). **⚠️ ALL Session-20 work below is UNCOMMITTED** —
34 changed files in the working tree (29 modified, 5 new). **Build:** ✅ app builds & launches.
**Tests:** ✅ 633/633 ctest.

## ⚠️ FIRST THING NEXT SESSION: commit the Session-20 stack
Nothing this session is committed. Recommend committing in logical chunks (one feature each),
NOT one giant commit. Suggested order (ADR + code per chunk):
1. **QM-073** viewport zoom/pan + aspect canvas (ADR-041) — shaders `quad/outline/handle.vert`,
   `Renderer.*`, `ViewportEditor.cpp`, `EditorState.hpp`, `Application.cpp`.
2. **QM-074** video file picker + crop L/R/T/B insets — `VideoPlayerPanel.cpp`, `InspectorPanel.cpp`.
3. **QM-075** Simple/Advanced modes (ADR-042) + auto-play + canvas drag-drop + click-to-select +
   Scenes-New seed quad — `MainMenuBar.*`, `PreferencesDialog.*`, `MediaAssign.hpp` (new),
   `MediaBinPanel.*`, `QuadListPanel.*`, `ScenePanel.cpp`, `QuadManager.*`, `Application.cpp`,
   `EditorState.hpp`, `InspectorPanel.cpp`, `VideoPlayerPanel.cpp`.
4. **QM-076** media-bin video thumbnails + hover-to-play — `MediaBinPanel.*`.
5. **QM-077** undo/redo (geometry + rename + grid edits) — `EditorState.hpp` (CommandHistory),
   `GridUndo.hpp` (new), `Application.cpp`, `MainMenuBar.cpp`, `ViewportEditor.cpp`,
   `InspectorPanel.cpp`, `QuadListPanel.cpp`.
6. **QM-078** Report Bug button + smooth corner-pin tessellation + bezier corner edges (ADR-043) —
   `QuadGeometry.*`, `Renderer.*`, `ViewportEditor.cpp`, `InspectorPanel.cpp`, `ProjectSerializer.cpp`,
   `MainMenuBar.cpp`.
> Exclude incidental churn: `imgui.ini` and `project.qmap` (layout/scratch) — don't commit as feature work.
> `CropRenderTest.cpp` change belongs with QM-073 (shaders gained `u_ViewScale/u_ViewOffset`; the test sets them).

## What shipped this session (built + ctest-green; ⚠️ user-visual-verify the interactive bits)
- **QM-073 — Viewport zoom/pan + aspect canvas (ADR-041).** Canvas renders into the dockspace
  central node, letterboxed to the assigned output's aspect (portrait auto via effective res);
  Fit/±/wheel zoom + middle-drag pan; success-metric hit-test inverts the same transform.
- **QM-074 — Video picker + crop insets.** "Load Video…" opens a native file dialog; crop UI is
  now Left/Right/Top/Bottom % insets (storage stays `{x,y,w,h}`); "Right ½"/"Left ½" align two faces.
- **QM-075 — Simple/Advanced UI (ADR-042).** Toolbar toggle (persisted, default Simple). Simple =
  curated Inspector/Video-Player + Media-only Resources + toolbar Apply-Test-Grid / Send-to-Projector.
  Plus: video auto-plays on assign; **drag clip from bin onto canvas quad** (manual
  `GetDragDropPayload`+release — custom target can't sit over the passthrough node); **click a quad
  to select it**; centralized `assignMediaToQuad` (fixed a video-as-image bug); Scenes "New" seeds a quad.
- **QM-076 — Bin thumbnails.** Video first-frame thumbnails (one-shot decode→cached texture),
  procedural live-texture thumbnails, and **hover-to-play** (one dedicated preview decoder).
- **QM-077 — Undo/Redo.** `CommandHistory` in `EditorState`; Ctrl+Z/Y + Edit menu; routes
  corner/mesh drag+nudge, rename, and grid edits (add/del col/row, even-grid, click-insert). Safe
  via a guard that **clears history on quad delete / scene switch** (commands hold raw pointers).
- **QM-078 — Report Bug + smooth distortion + bezier corners (ADR-043).** Toolbar "Report Bug"
  (opens the GitHub bug template). Corner-pin quads tessellate 32×32 → no diagonal fold. **Bezier
  edges on the 4 corners**: Coons patch via `surfacePoint`, amber drag handles, toggle + Reset in
  Inspector (Simple **and** Advanced), `.qmap`-persisted (additive), undoable.

## NEXT TASK — resume here
1. **Commit the stack** (see above) — do this first.
2. **🚩 User visual-verify** (couldn't auto-drive the UI reliably — shared desktop): smooth
   distortion (extreme corner-pin), bezier handles (toggle Curved edges → drag amber handles),
   hover-to-play, undo/redo, scene-New, canvas drag-drop, click-select.
3. **QM-079 — bezier on grid division lines (DEFERRED).** Corner bezier is done; curving interior
   division lines is a larger surface-model change. The user explicitly wants it next.
4. **Revert test pref:** `preferences.json` `defaultMediaFolder` was set to `D:\testing Videos`
   for verification (auto-loads 17 clips each launch). Clear it if unwanted.
5. ADR-040 dev→master PR still gated on 🚩 QM-061 (hardware). dev unpushed.

## Verification gate (non-negotiable)
`cmake --build build/debug` → `ctest --test-dir build/debug` (633/633) → success-metric warp
(corner drag + Tab/arrow + F11 projector). **Build gotcha:** the running app locks
`QUADmapper.exe`; close it before relinking. Shader edits need `cmake --preset debug` to re-embed.

## Reference links
- Decisions this session: @docs/adr/0041-viewport-zoom-pan-and-canvas-aspect.md ·
  @docs/adr/0042-simple-advanced-ui-modes.md · @docs/adr/0043-smooth-tessellation-and-bezier-edges.md
- Tasks: @TASKS.md · Machine state: @SESSION_STATE.json · Success metric: @CLAUDE.md#success-metric

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
