# ADR-034: Decompose `Application::render()` over the live model (not the source-arch shells)

**Date:** 2026-06-16
**Status:** Accepted
**Deciders:** Jones

## Context

TD-02's remaining work: pull the ~1900-line body of `Application::render()`
([Application.cpp:448-2358](../../src/app/Application.cpp#L448)) into real panel classes.
Step 1 (statics → owned `EditorState`) and step 2 (retire dual-track scene state, ADR-033) are
done. What's left is the UI decomposition.

Investigation (Session 11) found `render()` is a clear sequence of separable blocks:

| Lines | Block | ~LOC | Target |
|---|---|---|---|
| 452-500 | `EditorState` alias boilerplate | 50 | shrinks as blocks move out |
| 503-562 | Init + video-frame pump (logic, not UI) | 60 | stays in `render()` |
| 564-675 | Render all quads to main framebuffer | 111 | stays (draw step) |
| **676-897** | **On-canvas corner-pin + mesh-edit interaction** | **221** | `PreviewPanel` — ⚠️ **success metric** |
| 897-936 | DockSpace setup | 39 | stays |
| 941-1150 | Main menu bar (File/Edit/Window) | 210 | `MainMenuBar` (new) |
| 1153-1520 | Quad List panel | 367 | `QuadListPanel` (new) |
| 1523-2113 | Properties panel (quad/output/media) | 590 | `InspectorPanel` |
| 2116-2204 | Resources panel (tabbed) | 88 | already delegates to `MediaBinPanel`/`OutputPanel` |
| 2207-2338 | Video Player panel | 131 | fold into `InspectorPanel` video controls |

**The fork.** The two pre-existing shells (`InspectorPanel`, `PreviewPanel`) were written against
a **`SourceManager`/`TextureSource`/`MediaItem.mediaId` source model** that the live `render()`
never adopted:

- `InspectorPanel::renderQuadProperties/renderVideoControls` are fully written but **never
  called** — `render()` only ever calls `renderNoSelection()`. The live Properties panel uses the
  **legacy model**: a `quadTextures` map + direct `Texture::loadFromFile` + a single
  `EditorState::videoPlayer` ([Application.cpp:1581-1612](../../src/app/Application.cpp#L1581)).
- `PreviewPanel` renders into a docked ImGui child canvas with its own zoom/pan and **stubbed**
  `renderSceneContent/handleMouseInput` ("No scene connected"). The live editor warps directly on
  the fullscreen GLFW viewport background (676-897) — a different rendering surface.

So "wire up the shells" silently means "finish a half-done migration to the source architecture",
touching the texture/video assignment path **and** the on-canvas editing surface — i.e. the
success metric (`Application.cpp:674-808`, the no-regress gate). That is a materially larger,
riskier job than the decomposition TD-02 actually names.

## Decision

Decompose `render()` into panel classes **over the existing live `EditorState` model**. Pure
behavior-preserving extraction; do **not** migrate to the source architecture as part of TD-02.

1. **Extract blocks into panels** that take the live state, lowest-risk first, one per
   build→test→commit batch:
   `MainMenuBar` → `QuadListPanel` → Properties (`InspectorPanel`, rebuilt on the legacy model,
   video controls folded in) → `PreviewPanel` (viewport editing) **last**, since it owns the
   success-metric UX.
2. **Rebuild `InspectorPanel` against `EditorState`** (`quadTextures`/`videoPlayer`), replacing
   its unused `SourceManager`-based body. Keep `PreviewPanel` as the home for the existing
   viewport corner/mesh editing rather than its stubbed child-canvas approach.
3. **Delete the dead source-based shell code** that no panel calls (git is the backup), per the
   zero-debt / no-parallel-path rule and the ADR-033 precedent.
4. **Verify the smoke harness** (`tests/smoke/drive_smoke.ps1`) + full `ctest` after **each**
   batch; the vertex-warp + F11 projector workflow must stay green throughout.
5. **Track the source-architecture convergence as a separate, explicitly-scoped item**
   (`TD-09` in `TECHNICAL_DEBT.md`), not bundled into TD-02.

## Consequences

- ✅ `render()` drops from ~1900 lines to a thin orchestrator (~400) with real, testable panels.
- ✅ Success metric untouched: no change to the assignment path or warp surface during extraction;
  `PreviewPanel` keeps the verified viewport editing.
- ✅ Removes the dead source-based shell code (no parallel path), satisfying the zero-debt rule.
- ✅ Each batch is independently reversible and harness-verifiable (incremental discipline).
- ❌ Entrenches the legacy `quadTextures`/`videoPlayer` model and discards well-written
  source-based panel code. Accepted: it is dead today, git retains it, and the convergence is
  tracked as TD-09 — to be done as a deliberate change, not smuggled inside a refactor.
- ❌ The media path stays half-and-half (`MediaBinPanel` uses `SourceManager`; quad assignment
  uses `quadTextures`). TD-09 owns closing that gap.

## Alternatives considered

1. **Finish the source migration, then decompose** (un-stub `PreviewPanel`, call the existing
   `InspectorPanel` as written). Rejected for TD-02 — ~2× the scope, blows the ~2-session budget,
   and directly risks the success metric by rewriting the assignment + on-canvas-edit code under
   cover of a "decomposition". This is the scope-creep the reviewer-mode rule warns against.
   Revisit deliberately as TD-09.
2. **Leave `render()` as-is, just delete the dead shells.** Rejected — removes the parallel path
   but leaves the god-function, which *is* the debt TD-02 names.
