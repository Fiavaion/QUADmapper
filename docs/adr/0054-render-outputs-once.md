# ADR-054: Render each output once per frame, and make every draw cheaper

**Date:** 2026-09-26
**Status:** Accepted (owner: "get this development done", 2026-09-26)
**Deciders:** Jones

## Context

The PRD §5 benchmark (QM-107, Release, RTX 3080 Laptop) shows 256 quads at 49–62 fps on one 1080p output and 33–45 fps on two. The cost is draw-call overhead, not pixels.
- About 1,280 draws per frame: 256 quads each drawn for the editor canvas, 2 output previews and 2 outputs.
- Each draw costs 7–12 µs, mostly about 22 `glUniform*` calls plus state changes.
- Each output adds a context switch.

Because the scene switch finishes in the next frame, its time (21 ms against a 16 ms target) is really one frame's cost. The per-quad mesh cache also holds an identical index buffer for every quad with only four corners, about 100 MB of VRAM at 256 quads on 4 outputs (TD-22).

The output windows already share the editor's GL context.

## Decision

1. **Draw each output's scene once per frame, into a colour texture owned in the shared context.**
   - Each output window draws that texture as one fullscreen quad. Output rotation and blackout apply at this step.
   - The output preview panel shows the same texture. A preview therefore costs one textured draw and shows exactly what the projector shows, including black for missing media. The editor canvas keeps the magenta checker for missing media.
   - The pixel result on a projector must be identical to today's. A readback test compares the old path with the new one; if the outputs use MSAA, the render target uses it too and is resolved.
2. **Cut the per-draw cost.**
   - Cache uniform values per program and skip unchanged uploads, or move each quad's uniforms into a uniform block updated only when the quad changes; the implementer picks whichever measures faster.
   - Sort draws to avoid redundant texture and program binds where the order doesn't matter visually. It never changes the z-order.
3. **Share one index buffer** among corners-only quads with the same tessellation (TD-22).
4. **Gates.**
   - Measured with `--bench` (Release): 256 quads on 2 outputs at ≥ 60 fps, uncapped p99, on this machine; scene switch p95 < 16 ms.
   - The stress harness's GL object counts stay flat. The success-metric workflow looks identical.

## Consequences
- ✅ Draw work scales with outputs, not outputs plus previews. Previews become exact.
- ✅ Scene switches drop with frame cost (TD-20).
- ❌ One extra render target per output (8 MB at 1080p, 33 MB at 4K).
- ❌ The preview no longer shows the missing-media checker; it matches the projector instead.

## Alternatives considered
1. **Refresh previews less often.** Rejected as the primary fix: it only hides the cost, and previews go stale during a live show. It stays available as a fallback.
2. **Full canvas → slice → output compositing (ADR-047).** Deferred: that is the multi-projector blending work. This ADR is its first step, with per-output targets.
