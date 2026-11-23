# ADR-058: Per-quad warp mode, Linear for new quads

**Date:** 2026-11-23
**Status:** Accepted (owner, 2026-11-23: "Yes, Linear for new quads")
**Deciders:** Jones
**Relates:** ADR-043 (the projective corner-pin base), ADR-049 (surface = base + residual), ADR-032 (format versions)

## Context
- Since ADR-043 the corner-pin base B is the homography of the unit square onto the four corners, so a flat surface is perspective-correct inside a quad.
- The owner snapped two tilted quads edge to edge with the UV test grid on both (hands-on round, 2026-11-23). The grid rows broke at the shared edge, though both quads have the same two corners there and the same eight rows.
- The cause: along an edge from corner i to corner j, the homography puts parameter t at `(w_i (1-t) P_i + w_j t P_j) / (w_i (1-t) + w_j t)`, with corner weights w taken from the whole quad. Two quads with different perspective space the same edge differently, so content only meets at the two corners.
- The bilinear patch puts parameter t at `mix(P_i, P_j, t)` on every edge, whatever the other corners do. Two quads that share an edge then agree along all of it. Inside a strongly keystoned quad its lines bow slightly, because it is not a projective map.

## Decision
1. **Each quad has a warp mode:** `WarpMode::PERSPECTIVE` (the ADR-043 homography) or `WarpMode::LINEAR` (the bilinear patch, the existing fallback for concave corners). It lives on `QuadGeometry`, so slices' warps carry it too (they stay Perspective; there is no slice control).
2. **Defaults:**
   - `QuadGeometry` defaults to Perspective. Everything that builds one directly (slices, AI calibration quads, test patterns, loaded files) is unchanged.
   - `QuadManager::createQuad` makes Linear quads. That covers every "add a quad" path: the first quad of a session or project, Quad List "+", the Scene panel, undoable create. A duplicate keeps its source's mode.
3. **Switching keeps the control vertices in place:** a meshed quad's residuals are re-expressed against the new base, as `convertBilinearOffsetsToResiduals` does on load. Explicit grid handles keep their stored derivatives (a small change in the curve near them is accepted).
4. **UI:** a Perspective / Linear choice in Properties > Geometry (Advanced) and in the Simple-mode quad panel, one undo step, disabled on a locked quad. Reset Quad leaves the mode alone.
5. **Format 3.5 (additive):** quad geometry `"warpMode": "linear"`, written only for Linear. Missing means Perspective, so every earlier file loads exactly as before. A 3.4 reader ignores the key and draws such a quad in Perspective.

## Consequences
- New quads that are snapped together line up along their shared edges.
- A Linear quad is not perspective-correct inside. On a strongly keystoned flat surface, choose Perspective.
- AI Warp (ADR-053) sets corners only. On a Linear quad its result is drawn linearly; it does not switch the mode.
- The renderer's buffer cache compares the warp mode, so a switch redraws at once.
- Bench and stress-harness quads come from `createQuad`, so they are Linear now; bench numbers from before this change are not like-for-like.
