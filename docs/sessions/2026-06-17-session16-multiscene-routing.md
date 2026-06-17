# Session 16 — 2026-06-17 — Mapping-editor fixes + multi-scene output routing (ADR-039 P1+P2)

**Branch:** `dev` → pushed `origin/dev` @ `54dcec0` (8 commits). `master` clean at `fb4c85b`.
**Gate:** 619/619 ctest throughout; app launches/edits; multi-output verified on 2 monitors.

## Narrative

The session began with user bug reports on the on-canvas mapping editor and grew into the
multi-projector feature.

### 1. Mapping-editor fixes (committed `cfb3214`)
The user reported "can't click/drag vertices; only the Properties dialog works", wanted a Reset
button, and that Click-add produced a "crazy image".

Diagnosed by **running the app and driving synthetic Win32 input** (not from the bug report's
stated cause):
- **Vertex dragging actually worked** — synthetic drags proved corner *and* mesh-vertex drag both
  move geometry. The real causes were (a) handles rendered as oversized 2:1 ellipses that
  overlapped into a blob field on a dense grid, and (b) an **armed "Click-add" mode that silently
  suppressed all dragging** and turned every click into a division insert (the screenshot showed
  it `[ON]`).
- Initial DPI theory (cursor ÷ framebuffer) was a real *latent* bug but a no-op on this machine
  (window == framebuffer at default size); fixed defensively anyway (cursor ÷ **window** size).

Fixes: aspect-corrected handle shader (`u_Aspect`) → round handles; smaller mesh dots; cyan
**mesh-grid wireframe** in edit mode (revealing division lines); **clean 1×1 default grid**
(`MIN_MESH_RESOLUTION=1`) so quads start with no internal divisions; **drag-to-position** division
insert (arm → press inserts → drag repositions via new `moveU/VDivision` → release commits +
auto-disarms; Esc cancels; `insertU/VDivision` now return the inserted index); prominent
**Reset Transform** button (the existing `resetUV()` surfaced). +3 control-grid unit tests;
updated MeshSubdivision validity test for MIN=1.

Stopgap tradeoff (user-approved): control grid still doubles as the render mesh, so a quad with
few divisions textures affinely until divisions are added / Tier-2 decouples them.

### 2. Multi-scene design (ADR-039, committed `234e883`)
User asked for multi-projector preview + routing: a project holds **multiple scenes**, each a
separate mapping **assigned to one output**; default scene auto-assigns to the 2nd monitor; live
per-output preview. Investigation found the Output system already does monitor enumeration,
per-output monitor assignment, and perform mode — but `renderToOutputs` drew **all quads to every
output** (no routing), and **multi-scene had been deliberately retired in ADR-033** *as unwired
dead code*, with the `Scene` primitive kept and the deleted machinery + v2 read path preserved in
git for exactly this revival. ADR-039 revives it as a wired feature; `.qmap` → additive v3.

### 3. P1 — runtime foundation (QM-063, committed `af708c3`)
`EditorState` previously held a single `Scene scene` with a fixed `QuadManager&`. Replaced with a
`std::vector<std::unique_ptr<Scene>> scenes` + `activeSceneId` and `activeScene()` /
`quadManager()` accessors that resolve the active scene and lazily guarantee one exists. All ~26
editor call sites migrated (`es.quadManager` → `es.quadManager()`, one `es.scene` →
`es.activeScene()`). Behaviour-preserving (one scene today); app verified launching + editing.

### 4. P2 — per-output routing (QM-064, committed `6c712a4`), HARDWARE-VERIFIED
- `Scene::assignedOutputId` (1 scene → at most 1 output; null = edit-only).
- `OutputManager::renderToOutputs(scenes, textures)` renders **each output's assigned scene**;
  unassigned outputs stay black. Routing rule extracted as pure, unit-tested
  `OutputManager::sceneForOutput(scenes, outputId)`.
- Startup creates a default output and routes the default scene to the **2nd monitor** (present:
  2 monitors → monitor 1; else monitor 0). F11 lazy-create becomes a no-op.
- **Verified on real 2-monitor hardware:** a temp hook built a 2nd scene (top-left quad) → 2nd
  output on monitor 0; synthetic F11 entered perform mode; a virtual-desktop capture showed
  **monitor 1 = scene 1's centred quad, monitor 0 = scene 2's top-left quad** — different content
  per projector. Temp hook removed; rebuilt; 619/619.

## Key decisions
- ADR-039: revive multi-scene as a **wired** feature; 1 scene → ≤1 output; restore deleted
  `SceneManager`/`ScenePanel`/`PreviewPanel`/v2-serializer from `669a05c^` as reference and wire
  properly (no inert parallel track).
- Live scenes live in `EditorState` (runtime "active scene of truth"); `Project` is the serialized
  DTO (gets scenes[] + outputs[] in v3, P5).
- Default scene → 2nd monitor via a default output created at startup.

## Files modified (highlights)
`src/shaders/handle.vert`, `src/renderer/Renderer.{hpp,cpp}` (round handles + mesh-grid wireframe),
`src/ui/panels/ViewportEditor.cpp`, `src/ui/panels/InspectorPanel.cpp` (picking, drag-insert,
reset), `src/scene/QuadGeometry.{hpp,cpp}` (1×1 default, insert-returns-index, move-division),
`src/scene/Scene.hpp` (assignedOutputId), `src/app/EditorState.hpp` (multi-scene accessor),
`src/output/OutputManager.{hpp,cpp}` (per-output routing + sceneForOutput), `src/app/Application.cpp`
(default output + scenes render call), `docs/adr/0039-*`, `tests/unit/scene/*`,
`tests/unit/output/OutputManagerTest.cpp`, tracking files.

## Handoff → next session
**Resume at QM-065 (P3: Scenes UI).** The engine works but isn't user-operable — no UI to create a
2nd scene or assign outputs. Build a Scenes panel (add/delete/rename/switch active + assign output)
with viewport/QuadList/Inspector following the active scene; revive `ScenePanel` from `669a05c^` as
a reference and wire it as the active-scene controller. Then P4 (QM-066: per-output live preview,
FBO→thumbnails, revive `PreviewPanel`) and P5 (QM-067: `.qmap` v3 scenes+outputs persistence +
v1/v2/v3 migration tests).

**Lessons:** (1) reproduce-first — instrument and watch, don't fix the bug report's stated cause;
(2) read a *retiring* ADR before reviving a feature (ADR-033 left the path); (3) synthetic Win32
input + virtual-desktop capture is reliable for GUI/mouse/multi-monitor verification; (4) `cmake
--build --target` can skip the exe relink — `touch` the .cpp to force it (verify by binary
timestamp / grep).
