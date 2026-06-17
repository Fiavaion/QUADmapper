# ADR-039: Multi-scene project model with per-scene output routing + live preview

**Date:** 2026-06-17
**Status:** Proposed
**Deciders:** Jones

## Context

QUADmapper can drive multiple output windows (one per monitor/projector) and already has the
infrastructure: monitor enumeration + hot-plug ([OutputManager]), per-output window with a
**monitor assignment**, resolution, rotation, enable, and perform mode (fullscreen per
monitor). **But every output renders the *same* full quad list** —
`OutputManager::renderToOutputs` ignores per-output content — so two projectors cannot show
different content. There is also no in-app preview of what each projector will show; the only
way to see an output is to go fullscreen on the physical projector.

The required workflow (user, 2026-06-17): a project holds **multiple scenes**, each scene is a
**separate mapping** (its own quads/groups/view) **assigned to one output**. Example: a box lit
by two projectors — Projector 1 (1080p) shows a 2-quad scene for the left faces; Projector 2
(4K) shows a 10-quad scene for the right faces. The app launches with one default scene (it
cannot exist with zero scenes); the default scene auto-assigns to the 2nd monitor, or the only
monitor if there is just one. The editor edits **one active scene at a time** (its own mapping
UI). A live **per-output preview** lets the user see each projector without the hardware.

Multi-scene previously existed and was **retired in ADR-033** — *but only because it was
unwired dead code*, never because the concept was unwanted. ADR-033 explicitly anticipated
this revival ("Revisit as a deliberate feature, not as debt cleanup"; "git retains the deleted
code, and the v2 read path remains"). The deleted `SceneManager`, `ScenePanel`, `PreviewPanel`,
and the `.qmap` v2 `scenes` serializer are recoverable from `669a05c^`. The `Scene` class
itself was **kept** and is fully self-contained (its own `QuadManager`, groups, view state, id,
name, active flag) — so the data primitive already exists.

## Decision

Reintroduce multi-scene **as a wired, first-class feature** (deliberately superseding
ADR-033's removal), with per-scene output routing and live preview.

1. **Project owns scenes.** `Project` holds an ordered list of `Scene`s (≥1 always) plus an
   active-scene id, instead of a flat quad list. `EditorState` edits the **active** scene; the
   viewport, Quad List, and Inspector all operate on `activeScene().getQuadManager()`. Switching
   the active scene swaps the whole mapping UI. (The single-`Scene` "source of truth" from
   ADR-033 becomes "the *active* scene of truth" — still one editable scene at a time, no
   parallel/inert structures.)

2. **Scene → output assignment.** Each scene carries an optional assigned output id
   (`Scene::assignedOutputId`). Cardinality: **one scene → at most one output**, and an output
   shows the one scene assigned to it (0 or 1). Unassigned scenes are edit-only (not projected).
   The assignment UI lives with the scene (a Scenes panel).

3. **Per-output content routing.** `renderToOutputs` renders, for each enabled output, **only
   the scene assigned to it** (its quads/textures), instead of the global quad list. Outputs
   with no assigned scene render black.

4. **Default scene auto-assigns to the 2nd monitor.** On launch / new project, the default
   scene's output targets monitor index 1 if ≥2 monitors are present, else monitor 0. Re-applied
   when monitors hot-plug and the assignment is still unset.

5. **Live per-output preview.** Each output renders its assigned scene to an offscreen
   framebuffer (FBO) once per frame; the texture is shown as a thumbnail in the UI (Scenes/
   Output panel and/or a Preview panel). Recover `PreviewPanel` from git as the starting point.

6. **`.qmap` format: additive v3, backward-compatible reads.** Writer emits a `scenes` array
   (each scene: id, name, quads, groups, view, assignedOutputId) + an `outputs` array (monitor
   assignment, resolution, rotation, enabled) — **major bumped to 3**. Readers still accept:
   v1.x (flat quads → wrapped as a single default scene), v2.x (legacy `scenes` → mapped), and
   v3.x. Existing v1 files therefore keep loading unchanged. Per ADR-032's single-version policy
   and the Change-Boundary rule, this is a versioned migration with backward compat (no data
   loss for old files).

7. **Implementation = restore + wire, not stub.** Recover `SceneManager`/`ScenePanel`/
   `PreviewPanel`/v2-serializer from `669a05c^` as a reference, but **wire them as the real
   source of truth** (the gap ADR-033 named) and trim anything that doesn't earn its place — no
   re-introducing an inert parallel track.

## Phased plan (build → test → checkpoint per phase)

- **P1 — Data model:** `Project` holds `vector<Scene>` + active id; `EditorState` edits the
  active scene; New/Save/Load reconciled through one Scene↔Project helper. Unit tests.
- **P2 — Output routing:** scene→output assignment; `renderToOutputs` renders the assigned
  scene; default scene → 2nd monitor. Verify two windows show different content.
- **P3 — Scenes UI:** Scenes panel (list/add/delete/rename/switch active + assign output);
  active-scene editing across viewport/QuadList/Inspector.
- **P4 — Live preview:** per-output FBO render → ImGui thumbnails.
- **P5 — Persistence:** `.qmap` v3 writer + v1/v2/v3 readers + migration tests.

Each phase ends with `cmake --build` + `ctest` green and the success-metric warp intact.

## Consequences

- ✅ Two+ projectors can show different mappings; the box workflow is supported.
- ✅ In-app preview removes the "plug in the projector to see anything" loop.
- ✅ Reuses the kept `Scene` primitive and recoverable git history; aligns with ADR-033's
  stated revival path.
- ❌ Reverses ADR-033's *removal* (intended) and re-touches Change Boundaries: `.qmap`/
  `ProjectSerializer` (mitigated: additive v3 + v1/v2 read), the `scene`/`project` public
  modules, `OutputManager`, and CMake target lists. Hence this ADR.
- ❌ Largest feature since the AI wedge; multi-phase. Risk to the success metric (single-quad
  warp + F11) is contained by editing exactly one active scene and keeping the warp/edit path
  unchanged — verified each phase.

## Alternatives considered

1. **Per-quad → output tag** (no scenes): simpler data model, but every output would share one
   canvas/view and one Quad List — no "separate mapping UI per projector." Rejected: doesn't
   match the required workflow.
2. **Rebuild multi-scene from scratch** ignoring git history: more work, loses the tested v2
   read path and `PreviewPanel`. Rejected in favour of restore-and-wire.
3. **Keep flat model, add output content filters:** bolts routing onto a single shared canvas;
   conflates "what I edit" with "what each projector shows." Rejected — scenes are the right
   unit per the user's model.
