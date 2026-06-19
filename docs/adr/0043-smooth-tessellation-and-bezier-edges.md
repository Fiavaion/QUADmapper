# ADR-043 — Smooth render tessellation + bezier edges

- **Status:** Accepted (corner bezier implemented; grid-line bezier deferred)
- **Date:** 2026-06-19
- **Task:** QM-078
- **Relates:** ADR-038 (Kantan grid), ADR-041 (viewport zoom/pan)

## Context

A corner-pinned quad is a single 1×1 control cell, so it rendered as **two triangles** and
showed a visible diagonal "fold" under extreme warps (user-reported with a screenshot). The
ADR-038 comment already anticipated a "Tier 2" that decouples render density from the control
grid. Separately, the user asked for **bezier (curved) edges** so a quad can bow to non-flat
surfaces, with draggable handles on the corners (and, later, the grid division lines).

## Decision

1. **Smooth tessellation (decoupled from the control grid).** `QuadGeometry::surfacePoint(u,v)`
   is the single render sampler. For `CORNERS_ONLY` quads the renderer tessellates a fixed
   **32×32** grid sampled from `surfacePoint` instead of emitting the control grid directly, so
   the bilinear (or bezier) patch renders smooth at any warp. `MESH_EDIT` quads keep the
   control-grid tessellation (the user subdivides those deliberately).

2. **Bezier edges on the 4 corners.** Optional cubic-bezier edges, stored as `bezierEnabled_`
   plus 8 control-point **offsets** from the straight-line 1/3 & 2/3 points (zero == straight,
   and the controls track corner moves). `surfacePoint` blends the four edge curves into a
   bilinearly-blended **Coons patch** when enabled — identical to bilinear when offsets are zero.
   Amber handles render on the canvas; dragging them bows the edges. Toggle + "Reset Curves" in
   the Inspector (Simple **and** Advanced). Each bend is one undo step (`SetPropertyCommand`
   over the 8 offsets). Persisted in `.qmap` **only when enabled** (additive, backward-compatible).

## Consequences

- 33×33 verts / 2048 tris per corner-pin quad — within the existing dynamic buffers; fine for 60 FPS.
- Bilinear corner-pin is smooth but not perspective-correct (no texture homography); acceptable —
  the goal was "smooth," and a true projective map is a later option.
- **Deferred: bezier on grid division lines** — curving interior divisions needs a richer surface
  model + tessellation than the corner Coons patch; tracked as a follow-up (QM-079).

## Acceptance criteria

1. An extreme corner-pin renders smooth (no diagonal fold).
2. "Curved edges (bezier)" toggle shows amber handles; dragging them bows the edges smoothly;
   Reset Curves / Ctrl+Z revert; curves survive save/load; pre-bezier `.qmap` files load unchanged.
3. `ctest` green; success-metric warp (corner drag / Tab / arrows / F11) intact.
