# ADR-049: One surface evaluator for corner pin, mesh warp and bezier grid lines

**Date:** 2026-09-27
**Status:** Accepted (owner: "get this development done"). Reserved by the foundation audit, 2026-09-25.
**Deciders:** Jones

## Context

A quad has two surface models:
- **CORNERS_ONLY** draws a projective surface (a homography of the four corners, QM-086) plus Coons-patch deviations for the bezier edges (ADR-043). It is tessellated 32×32 with projective texture coordinates.
- **MESH_EDIT** draws its control grid as two flat triangles per cell over a *bilinear* base. The stored mesh offsets are relative to that bilinear base.

As a result:
- **Toggling Ctrl+M on a keystoned quad makes the image jump** (TD-11): ~278 px at the centre at 1080p, because the bilinear and projective surfaces disagree inside the quad.
- **Mesh cells are flat.** The warp shows creases at the grid lines; nothing interpolates smoothly between control vertices.
- **Bezier handles on interior grid lines (QM-079)** cannot be added without a surface that interpolates curves.
- **Render density is tied to the control grid.**

## Decision

1. **One evaluator, `S(u, v) = B(u, v) + R(u, v)`, for both modes.**
   - **`B` is the base.** It is the projective corner surface plus the bezier-edge Coons deviation, exactly today's corner-pin surface.
   - **`R` is the control-grid residual.** Each control vertex stores its offset from `B` at its grid parameter. Between vertices, `R` is interpolated per cell with a bicubic Hermite patch. Tangents default to Catmull-Rom, so the warp is smooth with no creases; a tangent becomes explicit when the user edits a handle (see 3).
   - With all residuals zero, `S` equals the corner-pin surface exactly. Entering or leaving mesh edit changes nothing on screen.
2. **Saved files keep their control points.**
   - **Old files** store offsets against the bilinear base. On load they convert: absolute position = bilinear(corners, uᵢ, vⱼ) + offset, then residual = absolute − `B`(uᵢ, vⱼ). Every control vertex lands exactly where the file put it, within 1e-5.
   - **New files** store residuals against `B`, marked by an additive `"meshBase": "surface"` field (`.qmap` stays 3.3.0 with a version-history note). A missing field means the old bilinear base.
   - The image between control vertices of an old file changes, from flat triangles to a smooth warp. That is the intended fix, and it is noted in the release notes.
3. **Bezier handles on grid lines (QM-079).**
   - Every control vertex can carry explicit tangent handles along its row and its column, two per axis, one on each side. They are optional and stored only when edited. The four outer edges keep the existing ADR-043 bezier controls.
   - Handles show on the canvas in mesh edit like the corner bezier handles do.
   - The keyboard model stays: Tab cycles the vertices, a modifier-free key toggles handle selection on the selected vertex, and the arrows nudge the handle with Shift 10× and Alt 0.1×. Mouse drag works too.
   - Undoable through id-keyed commands.
4. **Render density is independent of control density.** The surface is tessellated at a fixed density per quad (at least 32×32, more for large on-screen quads, capped). Positions come from `S`. Texture coordinates keep the projective `q` of the base, so corner-pin perspective stays exact and residuals move pixels without breaking perspective.
   - The per-quad GPU mesh cache (QM-107/112) keys on a geometry revision.
5. **Gates.**
   - A readback test: toggling mesh edit on a strongly keystoned quad with zero residuals changes no pixel.
   - Old-file control vertices keep their absolute positions (fixtures v1.2 grid + offsets, and v3.x).
   - The success-metric keyboard workflow is unchanged.
   - `--bench=quads256` does not regress (Release, quiet machine).
   - Interior bezier handles are covered by unit tests on the evaluator (tangent continuity at vertices, exact interpolation at vertices) and by the keyboard workflow.

## Consequences
- ✅ Ends TD-11 and the CORNERS_ONLY/MESH_EDIT split in behaviour; delivers QM-079; mesh warps become smooth.
- ✅ One evaluator to test, used by rendering, hit-testing and the AI/camera solvers.
- ❌ Old files with mesh offsets render slightly differently *between* control points (smoother). Control points themselves do not move.
- ❌ More per-vertex data (optional tangents).

## Alternatives considered
1. **Keep two models and blend on toggle.** Rejected: the mismatch stays, and grid bezier still needs a new surface.
2. **Store absolute control positions.** Rejected: moving a corner would no longer carry the mesh with it. Residuals relative to `B` keep "move corners, mesh follows".

## Amendment (implementation, 2026-09-27)
What was built, and where it differs from the text above:
- **Evaluator.** `QuadGeometry::basePoint` is B (the former corner surface). `surfacePoint` and `sampleSurface` return S = B + R. R applies in MESH_EDIT only, so corner mode ignores a stored warp, as before. R is one bicubic Hermite patch per control cell. Tangents are one-sided per vertex: explicit where a handle was set, otherwise non-uniform Catmull-Rom (the central difference over the neighbouring lines, one-sided at the boundary). The twist is the cross-difference of the tangents. With all tangents automatic the surface is the tensor-product Catmull-Rom surface, C1 across every cell boundary. An explicit handle on one side leaves the other side automatic, so the line can kink there on purpose.
- **Handles (QM-079).** Stored as dR/du or dR/dv (`VertexTangents`, one bit per handle). They do not depend on cell widths, and they follow corner moves. A handle is shown in the bezier convention: the vertex plus a third of the adjacent cell's width times dS on that side. Only the selected vertex's handles are drawn: amber once set, dim while automatic, green when selected by keyboard. A boundary vertex has no handle pointing out of the grid; the outer edges keep the ADR-043 controls.
- **Handle key: `H`.** In mesh edit, `H` (`Shift+H` backwards) cycles from the vertex through each available handle and back to the vertex. `Tab` still cycles vertices and drops the handle selection. The arrows nudge the selected handle with the vertex steps (Shift 10×, Alt 0.1×). Nothing else uses a bare H: the editor's keys are Tab, the arrows, Delete, 1–9 and numpad 1–9 (scene take), F11, Esc, and Ctrl/Alt chords (Ctrl+S/Z/Y/A/M/B/,/[/], Alt+=/−/R). H is claimed with the other canvas keys, so a focused panel cannot take it; Ctrl+H and Alt+H are not the handle key. Keyboard and mouse edits go through `MoveGridHandleCommand` (id-keyed; the nudges of one handle merge into one undo step). The key logic lives in `app/MeshEditKeys.hpp`, so the whole workflow is tested without a window.
- **Tessellation density rule.** While S = B (corner mode, or mesh edit with no residual and no handle): 32 × 32 even segments, exactly as corner pin was drawn. Otherwise each control cell is split evenly into max(2, ⌈32 / cells⌉) segments per axis. Every control line is then a tessellation line, so the mesh is exact at the control vertices, and each axis has at least 32 segments; 64 cells give the cap of 128. Density does not scale with on-screen size, because one cached mesh feeds the editor and every output, which draw it at different sizes. Any 32 × 32 tessellation uses the shared index buffer (TD-22).
- **One tessellator.** `MeshSubdivision::generateMesh` produces the render mesh: positions on S, texture coordinates (u·q, v·q, q) from the base. `Renderer::tessellate` calls it; the renderer's separate corner and control-grid paths were deleted. The mesh-grid wireframe draws each division line along S.
- **Hit-testing.** `QuadGeometry::surfaceParamsAt` finds the render triangle under a point and interpolates (u, v) projectively, as the fragment shader does. Click-to-select (`QuadManager::quadAtPoint`) and click-to-insert a division use it. `inverseBilinear` and `generateBilinearPositions` were removed.
- **GPU mesh cache.** It keeps the full-state comparison of QM-107/112, now including the tangents, instead of a revision counter. The comparison cannot miss a change made through `getGeometry()`; a counter that each setter bumps could.
- **Inserting a division** gives the new vertices the residual R already has there, so they land on the current surface. The surface elsewhere can change slightly, because neighbouring automatic tangents now see a closer neighbour.
- **Files.** `"meshBase": "surface"` is written with `meshOffsets`. Offsets without it are converted on load (absolute = bilinear + offset, residual = absolute − B, after the bezier edges are loaded). All-zero offsets are not converted: a file without a warp stays on the corner surface. Handles are saved as `meshTangents`: `[{"vertex": i, "handles": [u−, u+, v−, v+]}]`, each `[x, y]` or null. An entry for a missing vertex, or for a handle pointing out of the grid, is dropped; a malformed entry fails the load. `.qmap` stays 3.3.0. Fixture `v3.3-mesh-keystone.qmap` (keystone, bezier edges, offsets, no `meshBase`) was added.
- **Gates.** Readback, fixtures, evaluator, keyboard and undo tests pass (full debug ctest 1250/1250). The `--bench=quads256` gate is not yet shown: the one Release run was on a busy machine (other agents compiling, CPU 91–100%) and gave 112 fps on 1 output and 77 fps on 2 outputs, against 247–284 and 160–207 quiet. It needs a quiet-machine rerun.
- **Bench gate met.** Quiet-machine Release bench (2026-09-27, dev b3b20c9; nothing else building, background CPU ~29%: WSL, VS Code, Defender), with ADR-047's canvas pass on top: `--bench=quads256` 242 fps (p99 8.6 ms) on 1 output and 170 fps (p99 10.8 ms) on 2, against 247–284 and 160–207 before ADR-049. No regression.
