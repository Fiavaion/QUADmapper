# ADR-059: Grid warp in both modes, Linear / Spline points, Crop

**Date:** 2026-11-23
**Status:** Accepted (owner, 2026-11-23: "Yes, extend .qmap")
**Deciders:** Jones
**Relates:** ADR-038 (non-uniform control grid), ADR-049 (surface = base + residual), ADR-058 (warp mode), ADR-032 (format versions)

## Context
- The owner asked for:
  - a grid warp they can switch on per quad, with handles shown;
  - divisions added from the quad panel;
  - Click-add that also works on the quad's edge;
  - grid lines that can be dragged;
  - Linear or Spline points, per point and for new lines;
  - a setting where the bent outline crops the image instead of warping it.
- The grid warp (mesh edit, ADR-038/049) was in Advanced mode only, and every point was a Catmull-Rom spline.

## Decision
1. **One Grid warp panel** (`InspectorPanel::renderGridWarpControls`) in Simple and Advanced mode:
   - the Grid warp switch (mesh edit);
   - Add Column / Add Row, each at the middle of the widest column or row. The Insert pos slider is gone; Click-add places a line exactly.
   - Del Column / Del Row and the Even Grid slider;
   - Click-add, with the Spline / Linear choice for new lines;
   - All points Spline / Linear, and the selected point's type;
   - Image: Warp / Crop, and Reset Mesh.
2. **Linear points:** a `linear` flag per control vertex, kept in `VertexTangents`, so insert and remove carry it. In the residual field, a Linear point's automatic tangents are the one-sided chords to its neighbours, and its twist in each cell is that cell's bilinear twist.
   - Between two Linear points a grid line is straight in the residual. A cell with four Linear corners is exactly bilinear.
   - Making a point Linear drops its explicit handles. Dragging a handle makes it Spline again.
   - Reset Mesh keeps each point's type.
   - On a Perspective quad the base is projective along an edge, so a Linear line bends slightly with the perspective.
3. **Crop** (`ContentMode::CROP` on `QuadGeometry`):
   - The image is sampled on the plain corner pin, with no curved edges and no residual. Each render vertex carries a second coordinate: the plane point under it, found by Newton's method, as (s·q, t·q, q).
   - The soft edge still follows the surface's own (u, v).
   - quad.frag discards fragments outside the image when `u_ImageCut` is set.
   - With nothing bent, Crop draws exactly as Warp.
4. **Canvas:**
   - Click-add accepts a press within 1.5% of the outline (the nearest edge point's parameters).
   - In mesh edit, a press within 0.8% of an interior grid line drags that line. The mouse is mapped through the quad as it was when the drag began, so the line does not chase itself. The drag is one undo step.
   - Right-clicking a point opens its Linear / Spline menu.
5. **Test grid on new quads** is on by default and is a machine preference (`general.testGridOnNewQuads`), with a toggle in the menu bar. New quads in every scene get it. A loaded show's quads are marked seen, so a saved blank quad stays blank. The preferences also keep the new-points type and the crop units.
6. **Format 3.6 (additive):**
   - `"meshLinear": [vertex indices]`, written only when there is a Linear point.
   - `"contentMode": "crop"`, written only for Crop.
   - Missing means Spline and Warp. A 3.5 reader ignores both.

## Consequences
- The render vertex is 8 floats (was 5). mesh3d.vert passes its own coordinate through, so objects are unchanged.
- The renderer's buffer cache compares the content mode, and the Linear flags through `VertexTangents`.
- Crop costs a Newton solve per render vertex, only while the quad is bent and in Crop, and only when its geometry changes.
