# ADR-033: Retire the unwired multi-scene track (one scene of truth)

**Date:** 2026-06-16
**Status:** Accepted
**Deciders:** Jones

## Context

TD-02 step 2 calls for "one scene of truth." Investigation (Session 10) found the live editor
actually carries **three** representations, and the multi-scene machinery is built but
**completely unwired**:

- `m_editorState->scene` (a single `Scene` + its `QuadManager`) — the real source of truth:
  what `render()` edits and what `OutputManager::renderToOutputs` pushes to projectors
  ([Application.cpp:411](../../src/app/Application.cpp#L411),
  [:2449](../../src/app/Application.cpp#L2449)).
- `m_project` (flat `Project`) — what save/load actually persists; all four save/load sites use
  the flat `ProjectSerializer::save/load`, hand-syncing fields to/from the live `Scene`
  ([:919-974](../../src/app/Application.cpp#L919)).
- `m_sceneManager` (`SceneManager`) — **inert**: allocated at init, `reset()` at shutdown, one
  commented-out `update()`. Never edited, rendered, or saved.

The multi-scene feature set — `SceneManager` (+35 tests), `ScenePanel`, the `.qmap` v2 `scenes`
array, and `ProjectSerializer::{saveWithScenes,loadToSceneManager,serializeScenes,
deserializeScenes}` — exists but `ScenePanel::setSceneManager()` is never called and the panel is
never shown. Likewise `InspectorPanel`/`PreviewPanel`/`Renderer::renderScene` carry orphan
`SceneManager` setters/stubs with their parameters commented out. There is no way for a user to
create or switch scenes today. The app only ever *writes* the flat v1.0.0 format.

Keeping a fully parallel, non-functional subsystem violates the project's zero-technical-debt
rule and is exactly the kind of dead/parallel path that breeds drift (the same buggy
"copy corners/resolution/mode only" quad-copy is duplicated across the scene loader *and*
render()'s load path, silently dropping mesh-edit offsets — see ADR-032).

## Decision

Delete the unwired multi-scene track and make the single `Scene` the one source of truth.

1. **Remove `SceneManager`** (`src/scene/SceneManager.*` + `SceneManagerTest.cpp`) and the
   inert `Application::m_sceneManager` member, accessor, init and reset.
2. **Remove `ScenePanel`** (`src/ui/panels/ScenePanel.*`) — never instantiated.
3. **Remove the orphan `SceneManager` hooks** from `InspectorPanel`, `PreviewPanel`, and the
   unused `Renderer::renderScene(const SceneManager&)` stub.
4. **Remove the `SceneManager`-based serializer methods** `saveWithScenes`,
   `loadToSceneManager`, `serializeScenes`, `deserializeScenes` and their scene-only static
   helpers (`serializeScene`, `deserializeScene`, `loadSceneContents`, `loadScenesArray`,
   `loadV1QuadsAsScene`, scene view-state helpers).
5. **Keep the flat reader/writer and ADR-032's version policy.** `validateVersion`,
   `load`/`deserialize`/`save`/`serialize`, `loadFlatProject` (incl. its v2-`scenes`→flat
   flatten branch) and `loadQuadIntoProject` stay. The flatten branch means any stray v2 file
   still loads (its scene quads collapse into the flat model). The `.qmap` on-disk format the
   app writes is unchanged (flat, `1.0.0`).
6. **Reconcile persistence** — extract render()'s hand-rolled `Scene`↔`Project` field copies
   (New/Save/Load sites) into one shared helper pair that copies the **whole** geometry, fixing
   the mesh-offset drop that the manual copies share, and removing the duplication.

## Consequences

- ✅ One source of truth (`EditorState::scene`); the inert third structure is gone.
- ✅ Removes a large non-functional subsystem and its drift risk; mesh offsets now survive
  save/load through the shared copy helper.
- ✅ `.qmap` simplifies to a single (flat) format the app actually writes, while still reading
  legacy v2 files via the flatten path.
- ❌ Drops multi-scene as a (currently non-functional) capability. Re-introducing it later
  means rebuilding `SceneManager`/`ScenePanel` wiring — acceptable: git retains the deleted
  code, and the v2 read path remains, so a future multi-scene effort can restore from history
  rather than from a half-integrated stub.
- ❌ Touches Change Boundaries (`ProjectSerializer`/`.qmap`, the `scene` public module, CMake
  target file lists) — hence this ADR. Mitigated by: no on-disk format change, the v2 read
  path preserved, and full build + ctest after each deletion/reconcile batch.

## Alternatives considered

1. **Wire up multi-scene instead** (promote `SceneManager` to source of truth, connect
   `ScenePanel`, switch persistence to `saveWithScenes`). Rejected for now — ~5× the scope and
   it commits the product to a feature that isn't currently required; the success metric is
   single-quad/single-output. Revisit as a deliberate feature, not as debt cleanup.
2. **Leave `m_sceneManager` parked, reconcile only the live/persistence split.** Rejected —
   leaves the dead subsystem and its drift risk in place; that *is* the debt TD-02 names.
