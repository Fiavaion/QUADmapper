# ADR-047: Scene canvas → output slices, with edge blending and scene crossfades

**Date:** 2026-09-27
**Status:** Accepted (owner: "get this development done"). Reserved by the foundation audit, 2026-09-25, as the top recommended next capability.
**Deciders:** Jones

## Context

Today a scene is assigned to at most one output (ADR-039). Its quads live in that output's normalized [0, 1] space, and each output draws its scene once per frame into its own frame texture (ADR-054).

Three things a live-event rig needs cannot be done:
- **Blending projectors.** Two or more projectors cannot share one wide or tall image with soft blended overlaps. That is standard in every competing tool, and the biggest capability gap in the audit.
- **Output-side correction.** An output cannot show just part of a scene, or warp its whole image (a projector that is slightly off-axis), without moving every quad.
- **Scene crossfades** (PRD US-105) need a composite stage where two scenes' images can be mixed.

TD-12 (per-output vsync) also belongs here.

## Decision

1. **Each scene has a canvas** in pixels.
   - Its quads and objects live in canvas space, normalized [0, 1] as today, so existing geometry needs no conversion.
   - A new scene's canvas takes its first output's resolution. Old files get the assigned output's resolution, or 1920×1080 when unassigned.
   - The canvas size is editable in the Scenes panel; quads keep their normalized positions.
2. **Each output shows slices of one scene.** The routing turns around: an output picks its scene, and several outputs may show the same scene. Old files map scene.assignedOutputId to output.sceneId.
   - A **slice** has:
     - an input rectangle on the canvas, in pixels;
     - an output warp: four corners in the output's normalized space, optionally a grid through the ADR-049 evaluator;
     - edge blend zones for left, right, top and bottom, each with width in pixels, gamma and a curve;
     - an optional black-level lift for the overlap.
   - An old file gets one full-canvas slice per output with no warp and no blend, which renders exactly as today.
   - Slices are edited in an Output panel slice editor, with the same keyboard model for the warp corners: Tab, the arrows, Shift and Alt.
3. **Auto-blend.** When two or more outputs show the same scene and their slices overlap on the canvas, one action sets matching blend widths on the facing edges. Values stay editable, and a blend test pattern is available.
4. **Rendering.**
   - Each scene that at least one output shows is drawn once per frame into a canvas texture at canvas resolution.
   - Each output then composes its slices from that texture into its frame texture (ADR-054) with a slice shader: warp, blend ramp, gamma, black level.
   - The output windows and previews are unchanged.
   - 3D objects render per output through that output's ProjectorCamera straight into the output frame, as today. Calibration is per projector, so objects are not drawn through the canvas.
   - Blackout and missing-media black apply as today.
5. **Scene takes and crossfades.**
   - A take (keys 1–9, OSC, MIDI) swaps the scene on every output that currently shows the active scene, so a blended group changes together.
   - Takes can carry a fade time: global in Preferences, per take through OSC `/qm/scene/take i n f seconds`, default 0 = cut. During a fade both scenes' canvases render and the slice shader mixes them. Audio follows the audibility rule, with both scenes audible and cross-gain ramped during the fade.
6. **Per-output presentation (TD-12).**
   - Measure first. If a second fullscreen projector tears with interval 0, present outputs that share a refresh rate with vsync from the single present point, and pace outputs at other refresh rates independently, e.g. one present thread per output that swaps the already composed frame.
   - GL composition stays on the main thread.
   - The chosen design and its measurements go in an amendment.
7. **Persistence.** `.qmap` 3.4 adds the scene `canvas` and output `sceneId` and `slices` fields. Readers accept 3.x. Old files load as described above and render identically; a fixture proves it.
8. **Gates.**
   - Pixel identity: an old show renders byte-identically on its outputs (readback).
   - Blend correctness: in a two-output overlap, the sum of the linear-light contributions is flat within 2% across the overlap for gamma 2.2.
   - The success-metric workflow is unchanged.
   - Takes change blended groups together, and `sameFrame` still passes.
   - A 1 s crossfade is smooth: no frame repeats, and the mix is monotonic.
   - `--bench=quads256` on 2 outputs stays ≥ 60 fps with p99 ≤ 16.7 ms on a quiet machine, with the extra canvas pass.

## Consequences
- ✅ Multi-projector blended shows, output-side keystone and scene crossfades.
- ✅ Old shows behave exactly as before.
- ❌ One extra render pass per shown scene: a canvas-sized texture, 8 MB at 1080p.
- ❌ Routing turns around (output → scene), touching the Scenes panel, scene takes, OSC and serialization; all covered by tests.

## Alternatives considered
1. **Blend per quad** (soft edges already exist, ADR-037). Rejected: it cannot blend two projectors showing one continuous image.
2. **One global canvas for the whole show.** Rejected: separate scenes per output, from ADR-039, remain a valid workflow.
