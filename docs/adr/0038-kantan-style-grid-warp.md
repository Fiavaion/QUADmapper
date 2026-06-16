# ADR-038: Kantan-style non-uniform grid warp (US-101-grid / #3)

**Date:** 2026-06-16
**Status:** Proposed — **awaiting sign-off before implementation**
**Deciders:** Jones

## Context

QUADmapper's mesh warp today is a **uniform N×N grid** ([QuadGeometry](../../src/scene/QuadGeometry.hpp)):
one `meshResolution_` (2–64) defines an evenly-spaced grid, and `meshVertices_` holds a
per-vertex offset from the bilinear position. Editing is the success-metric UX (Tab-cycle +
arrow-nudge + drag handles, mesh-edit mode) — see
[scene-geometry rules](../../.claude/rules/scene-geometry.md) and
[Application.cpp:674-808](../../src/app/Application.cpp#L674-L808). **This must not regress.**

The user wants TouchDesigner **Kantan Mapper**-style grid warping and to go *beyond* it:

- **Kantan** (researched): a quad with rows/columns; **Add Row / Add Column** inserts a
  division at an arbitrary clicked position (non-uniform); a row/column can be **deleted**
  preserving other deformations; **Linear** mode connects grid points with straight segments,
  **Bezier** mode warps with per-point keys+handles (smooth curves); per-cell texture mapping
  is bilinear or perspective.
- **Beyond Kantan** (the agreed differentiator): **local cell subdivision** — refine a single
  cell without the new lines spanning the whole quad (Kantan's rows/columns are always global).

A uniform grid can't express any of this. This needs a new geometry/mesh data model — a
**Change Boundary** (the `core`→`scene` geometry model *and* the `.qmap` format), hence this ADR.

## Decision (phased — each tier ships build→test→smoke green, success metric intact)

Replace the uniform-resolution mesh with a **control grid**: explicit, independently-positioned
division lines, per-control-vertex offset, and (later) Bezier handles; the **render mesh** is a
fine tessellation generated *under* the control grid (decoupled from it). Three tiers.

### Tier 1 — Non-uniform linear control grid (Kantan Linear parity)
- **Data model:** `QuadGeometry` gains `std::vector<float> uDivisions_` and `vDivisions_`
  (normalized 0..1 interior division positions; corners implicit at 0 and 1) plus a control-grid
  vertex-offset store keyed by (row,col). The current uniform `meshResolution_`/`meshVertices_`
  becomes a special case (evenly-spaced divisions) — kept as a migration source, not the model.
- **Ops:** insert a U or V division at a clicked edge position; delete a selected division
  (re-indexing offsets, preserving the others); drag a control vertex; reset.
- **Interp:** bilinear within each (generally non-square) control cell. Render mesh = per-cell
  fine tessellation so even Tier 1 looks smooth at cell interiors.
- **UX:** mesh-edit mode keeps Tab-cycle + arrow-nudge over **control** vertices (the success
  metric, now over a non-uniform set). New tools: "Add Row/Column" (click) + "Delete Line".

### Tier 2 — Bezier handles + explicit tessellation (full Kantan parity, smoother)
- Each control vertex gains optional **Bezier handles** (in/out tangents); a per-quad
  `gridMode ∈ {Linear, Bezier}`. Bezier cells evaluate a bicubic/Coons patch; tessellation
  density is a quad property (replaces the old single resolution, now a *render* knob).
- Handle editing in the ViewportEditor (drag key → drag handles), matching Kantan.

### Tier 3 — Local cell subdivision (**beyond Kantan**)
- Allow subdividing a **single cell** into a finer sub-grid without propagating lines across
  the quad → an **adaptive (quadtree-like) control mesh**, not a pure row/column lattice.
- **The hard part — T-junctions:** a fine cell adjacent to a coarse edge leaves a hanging
  vertex → visible cracks/seams. Mitigation: **edge-stitching** (triangulate the coarse edge to
  include the neighbour's hanging vertices) or constrained Delaunay over the control points.
  This is the bulk of Tier 3's cost and risk and gets its own verification (a crack-free render
  test on an adaptively-subdivided quad).

### Persistence (`.qmap`)
- New optional `grid` object on a quad: `{ uDivisions[], vDivisions[], mode, vertices[] (offset
  + optional handles), subdivisions[] (Tier 3) }`. Written only when the quad uses the new grid
  (a plain corner/uniform quad stays as today). Old files: absent → migrate the uniform
  `meshResolution_`/`meshVertices_` into evenly-spaced divisions on load (lossless).
- Additive MINOR bump (ADR-032 §5): the unreleased 1.1 line → **1.2.0** when Tier 1 lands. Old
  builds ignore the unknown `grid` key (they'd lose the non-uniform warp — acceptable forward-
  incompat for a new feature; major unchanged so the file still opens).

### Render path
- A new `QuadGeometry::generateRenderMesh()` emits vertices+indices from the control grid
  (Tier 1 bilinear, Tier 2 patch eval, Tier 3 stitched). `Renderer`/`Mesh` already upload an
  arbitrary indexed mesh, so the renderer change is localized to mesh generation. Stays within
  GL 3.3. 60-FPS budget: tessellation is rebuilt only on edit, cached otherwise (same as today).

## Consequences

- ✅ Kantan parity (Tier 1+2) plus a genuine differentiator (Tier 3 local refinement).
- ✅ Decoupled control/render mesh → smooth warps from few control points; better authoring.
- ✅ Backward compatible: existing uniform-mesh quads migrate losslessly; success-metric UX
  preserved (Tab-cycle + nudge now over control vertices).
- ❌ Largest change to `QuadGeometry` since inception — touches scene model, serializer,
  ViewportEditor (success-metric-critical), Inspector, and 30+ existing `QuadGeometry` tests
  that assume a uniform grid. Phased delivery + migration keep each step verifiable.
- ⚠️ Tier 3 T-junction stitching is the real risk; if it proves costly it can be deferred
  without losing Tier 1+2 value (Tiers are independently shippable).
- ⚠️ Commands/undo (`MoveMeshVertex`) and any mesh-offset (row,col) indexing assume a fixed
  grid size — they need to handle insert/delete re-indexing. Folded into Tier 1.

## Alternatives considered

1. **Keep the uniform grid, just raise the resolution cap.** Rejected — doesn't give targeted
   control where the surface needs it (the whole point), and dense uniform grids are unwieldy
   to hand-warp.
2. **Free-floating control points (no row/column structure).** Rejected as the base model —
   loses the predictable cell interpolation and the familiar Kantan workflow; Tier 3's local
   subdivision already provides controlled irregularity where wanted.
3. **Per-cell perspective vs bilinear toggle (Kantan has it).** Deferred to a Tier-2 follow-up;
   bilinear-per-cell with fine tessellation covers the common case first.

## Open questions for sign-off

1. **Tier order / stop point:** confirm Tier 1 → 2 → 3 sequential, each merged before the next?
2. **Tier 3 now or after a Tier-1+2 field test?** (Recommend: build 1+2, validate the workflow,
   then commit to Tier 3's T-junction work.)
3. **Replace vs. coexist:** make the new control grid the *only* mesh model (migrate uniform
   away), or keep "uniform resolution" as a simple preset alongside it? (Recommend: migrate to
   one model with an "even N×N" convenience action, to avoid two mesh code paths / TD.)
