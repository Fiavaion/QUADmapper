# ADR-037: Per-quad soft-edge blending (US-101)

**Date:** 2026-06-16
**Status:** Accepted
**Deciders:** Jones

## Context

Multi-projector setups overlap two (or more) projections on the same surface region.
Without blending, the overlap is a visible bright seam. **Soft-edge blending** feathers each
projection's edge so the overlapping ramps sum to uniform brightness — the standard
edge-blend technique.

QUADmapper renders each surface as a warped quad through a per-quad fragment shader
([quad.frag](../../src/shaders/quad.frag)) that already applies opacity, UV transforms, and
(ADR-036) color correction. The overlap is authored as two quads covering the shared region;
feathering belongs **per quad, per edge** (only the edge that overlaps is feathered).

This adds persisted per-quad parameters, so the `.qmap` schema gains a field — additive,
backward-compatible, same reasoning as ADR-036.

## Decision

1. **Data model.** Add a `SoftEdge` value struct (in [Quad.hpp](../../src/scene/Quad.hpp))
   held by each `Quad`, identity = off (renders byte-identically to today):
   - `left`, `right`, `top`, `bottom` — feather width per edge as a fraction of the quad,
     range `[0, 0.5]`, default `0` (no feather on that edge).
   - `gamma` — falloff curve for the ramp, range `[0.1, 4]`, default `1` (linear).

   Provides `reset()` and `isIdentity()`; the setter clamps to the ranges above.

2. **Shader.** `quad.frag` gains five uniforms. Using the **quad-local** `v_TexCoord`
   (0..1 across the quad, independent of texture pan/zoom — see [quad.vert](../../src/shaders/quad.vert)),
   it computes a per-edge linear ramp (`clamp(dist/width)`), multiplies the four ramps, raises
   the product to the power `gamma` (the falloff curve), and multiplies that into alpha
   **after** color correction. A zero width disables that edge (ramp = 1), so identity is a
   true no-op. Stays within the GL 3.3 / GLSL 3.30 floor.

3. **Render path.** `Renderer::renderQuad` sets the five uniforms from `quad.getSoftEdge()`.
   No new draw calls or passes — the same single textured draw per quad (60-FPS budget safe).
   Overlapping feathered quads blend via the existing alpha blend already used for opacity.

4. **Persistence.** `serializeQuad` writes an optional `softEdge` object, only when non-identity;
   `deserializeQuad` reads it if present (old files → off). No writer-version change: this ships
   in the same unreleased **1.1** dev line as ADR-036's `colorCorrection` (both additive,
   major unchanged, old builds ignore unknown keys).

5. **UI.** `InspectorPanel` gains a "Soft Edge Blending" section (per-edge width sliders +
   gamma + Reset), mirroring the Color Correction section, flagging the project dirty on change.

## Consequences

- ✅ Seamless multi-projector overlaps authored entirely with the existing quad model — no new
   output-blend subsystem, no extra framebuffer.
- ✅ Identity defaults render unchanged (smoke-verified); per-quad granularity matches how
   overlaps are actually set up (feather only the shared edge).
- ✅ One textured draw per quad — no 60-FPS regression.
- ❌ Touches the `.qmap` Change Boundary and the shared quad shader — hence this ADR. Mitigated
   by additive-only schema, identity no-op defaults, and a new round-trip test.
- ⚠️ Edge feathering is in quad-local UV space, so it tracks the quad geometry (warps with it),
   not screen-space. For overlapping projections of the same surface region this is the desired
   behavior; true screen-space output blend zones remain a future option (alternative 2).

## Alternatives considered

1. **Per-output (projector) blend zones** — screen-space left/right/top/bottom blend bands on
   each output window. Rejected for v1.1: QUADmapper's content lives in warped quads, not
   rectangular output regions; per-quad feathering matches the authoring model and reuses the
   per-quad uniform path. Per-output zones can be added later as a separate seam.
2. **Luminance-corrected blend curve (per-channel gamma + black-level lift).** Deferred — the
   gamma'd linear ramp covers the common case; black-level/per-channel correction can extend
   `SoftEdge` additively later (same MINOR-bump rule) once a real projector pair shows the need.
3. **Bake the feather into the texture/media.** Rejected — not adjustable live during setup,
   and breaks when the same media is reused on differently-overlapping quads.
