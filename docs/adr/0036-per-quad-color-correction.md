# ADR-036: Per-quad color correction (US-104)

**Date:** 2026-06-16
**Status:** Accepted
**Deciders:** Jones

## Context

v1.1 polish adds **per-quad color correction** so a technician can match the look of
content across projectors and surfaces (brightness/contrast/gamma/RGB balance) without
re-authoring the media. The natural home is the per-quad render path that already applies
opacity and UV transforms in the fragment shader ([quad.frag](../../src/shaders/quad.frag),
uniforms set in [Renderer::renderQuad](../../src/renderer/Renderer.cpp)).

This touches a **Change Boundary**: the new per-quad parameters must persist, so the `.qmap`
schema gains fields ([ProjectSerializer.cpp](../../src/project/ProjectSerializer.cpp)).
Per ADR-032, adding fields is an **additive MINOR bump**, not a breaking change.

## Decision

1. **Data model.** Add a value struct `ColorCorrection` (in [Quad.hpp](../../src/scene/Quad.hpp))
   held by each `Quad`, with identity defaults (so an untouched quad renders byte-identically
   to today):
   - `brightness` — additive, range `[-1, 1]`, default `0`.
   - `contrast` — multiplier about mid-gray (0.5), range `[0, 2]`, default `1`.
   - `gamma` — display gamma, range `[0.1, 4]`, default `1` (applied as `pow(c, 1/gamma)`).
   - `rgbGain` — per-channel multiply (white balance), range `[0, 2]`, default `(1,1,1)`.

   The struct provides `reset()` and `isIdentity()`. Setters clamp to the ranges above.

2. **Shader.** `quad.frag` gains four uniforms and applies, in a fixed order on the sampled
   RGB (before the existing alpha×opacity): **RGB gain → brightness → contrast → gamma**.
   With identity values every step is a mathematical no-op, so existing scenes are unchanged.
   Stays within the GL 3.3 / GLSL 3.30 floor (the rendering-rules ceiling).

3. **Render path.** `Renderer::renderQuad` sets the four uniforms from `quad.getColorCorrection()`.
   No new draw calls or state changes — same single textured draw per quad (60-FPS budget safe).

4. **Persistence.** `serializeQuad` writes an optional `colorCorrection` object; `deserializeQuad`
   reads it **only if present** (old files → identity defaults, exactly the existing per-field
   `if (j.contains(...))` pattern). The writer version bumps `1.0.0 → 1.1.0` (additive MINOR,
   ADR-032 §5). Old builds ignore the unknown key; the major is unchanged so `validateVersion`
   still accepts the file both directions.

5. **UI.** `InspectorPanel` gains a "Color Correction" section (sliders for each parameter +
   a Reset button), mirroring the existing opacity/UV controls and flagging the project dirty
   on change.

## Consequences

- ✅ Per-quad look-matching with zero impact on untouched scenes (identity defaults render
   byte-identically; verified by the success-metric smoke).
- ✅ `.qmap` stays backward- **and** forward-compatible (additive optional field, major
   unchanged); old `.qmap` files load with defaults.
- ✅ One textured draw per quad — no 60-FPS regression.
- ❌ Touches a Change Boundary file (`ProjectSerializer.cpp`) and a shared shader — hence this
   ADR. Mitigated by additive-only schema, identity no-op defaults, and a new round-trip test.

## Alternatives considered

1. **Color correction as a post-process over the whole output** (one LUT/pass per output).
   Rejected for v1.1 — the use case is matching *individual* surfaces; per-quad is the right
   granularity and reuses the existing per-quad uniform path with no new framebuffer.
2. **3D LUT (.cube) per quad.** Rejected as over-engineering for v1.1 — heavier UI, a new
   asset type, and a 3D texture upload path. The four scalar/RGB controls cover the matching
   use case; a LUT can be added later behind the same `ColorCorrection` seam if needed.
3. **Add a saturation/hue control too.** Deferred — kept scope to the requested
   brightness/contrast/gamma/RGB to avoid creep; saturation can extend `ColorCorrection`
   additively later (another optional field, same MINOR-bump rule).
