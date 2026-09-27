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

## Amendment 1 (part A implementation, 2026-09-27, QM-120)
Part A (§1, §2 data model, §4 rendering, §5 takes without fades, §7 persistence, the gates) is built. Part B is QM-121.
- **Model.** `Scene` has a canvas size in pixels (default 1920x1080, clamped to 16384). `Scene::assignedOutputId` and the scene-list routing helpers were removed: the routing is on the output (`OutputWindow::sceneId`), and each output holds its `Slice`s (`scene/Slice.hpp`). A slice is an input rectangle in canvas pixels, a warp (an ordinary `QuadGeometry` in output space, so a grid warp goes through the ADR-049 evaluator and `MeshSubdivision::generateMesh`), four `BlendEdge`s (width in canvas pixels, gamma, curve) and a black level. `OutputManager::showScene` routes a scene: an output without slices gets one full-canvas slice; existing slices are rescaled when the canvas size changes, so they keep their place on the picture.
- **Blend ramp.** Linear-light ramp f(t) = ½(2t)^p below ½, 1 − ½(2(1 − t))^p above, with p the edge's curve (default 2), so f(t) + f(1 − t) = 1 for any p. The pixel gain is f^(1/gamma), the product over the four edges. The black level is a lift `L + (1 − L)·c`, applied only outside every blend zone (a step, not a ramp). The shader and `sliceBlendGain` share the formula.
- **Rendering.** `OutputManager::renderOutputFrames` draws each scene that an output shows once into its canvas (RGBA8, cleared to transparent, colour blended exactly as before, coverage accumulated in alpha), then for each output: black clear, the scene's 3D objects through that output's projector, then its slices composited with (ONE, ONE_MINUS_SRC_ALPHA). The ADR-054/QM-115 ordering and blackout are unchanged; in blackout no canvas is drawn. Canvases are keyed by scene, reused across frames, released when no output shows the scene, when its last output is deleted and at shutdown. No per-frame allocation after the first frame.
- **Deviation: the canvas is drawn in its outputs' rotation.** When every output showing a scene has the same rotation the canvas is rasterized already turned (as the frame was before canvases), and the slice shader maps into it; with mixed rotations it is drawn unturned. Without this, a rotated projector differed by up to 2 levels at many pixels because the quads were rasterized in another orientation.
- **Deviation: exact-copy path.** An unwarped slice whose input is exactly the frame's effective size and starts on a pixel boundary fetches the texel under each pixel (`texelFetch`), with no filtering. Other slices sample bilinearly.
- **Gate a (pixel identity), measured by readback** (`RenderOnceReadbackTest`, the ADR-054 feature scene: keystone, bezier, mesh warp, crop, colour correction, soft edge, translucency, plus a 3D cube): with no 3D object, byte-identical at 0/90/180/270 degrees; in blackout, identical. With the cube, every pixel is identical except where a translucent quad lies over the cube: about 410 of 16,896 pixels differ by at most 2 levels (1 at 0 degrees). The old path blended the quad straight onto the object; the canvas blends it into 8 bits first and composites that over the object, two roundings instead of one. Objects cannot go through the shared canvas because each projector sees them from its own camera. Accepted; a 16-bit canvas would fix it but can break exactness for the common no-object case.
- **Gate b.** `SliceTest.TwoOutputOverlapSumsToFlatLinearLight` (maths, < 1e-4) and `CanvasSliceRenderTest.TwoOutputOverlapSumsToFlatLinearLight` (GPU readback, 8-bit frames, gamma 2.2, 128 px overlap): worst |sum − 1| = 0.0053, within the 2% gate.
- **Gate c/d.** The keyboard-workflow tests are unchanged and pass. Takes: if an output already shows scene N it just becomes active (unchanged); otherwise N replaces the active scene on every output that shows it (the blended group); with none, on the first enabled output. `SceneTakeTest.BlendedGroupChangesTogether`, `SceneTakeRoutingTest.*`; the `sameFrame` bench scenario is unchanged.
- **Gate e (bench).** Release `--bench=quads256 --bench-outputs=2` ran on a busy machine (another agent compiling, CPU 54–100%): 1 output 48.6 fps, p99 98.9 ms; 2 outputs 67.1 fps, p99 44.7 ms. Not a quiet-machine result; the ≥ 60 fps / p99 ≤ 16.7 ms gate still needs a quiet rerun.
- **Migration.** Files before 3.4: output.sceneId comes from the (first) scene whose assignedOutputId named it; that scene's canvas is the output's effective (rotated) resolution, else 1920x1080; the output gets one full-canvas slice. Outputs no scene named show nothing and get no slice (the ADR text says one slice per output; an unrouted output has nothing to slice, and `showScene` gives it one when it is routed). `.qmap` 3.4.0 still writes scenes[].assignedOutputId (first output showing the scene) so 3.0–3.3 readers keep their routing. Fixture `v3.4-canvas-slices.qmap`; every older fixture is checked to migrate to one unwarped, unblended full slice.
- **Canvas follows a lone output.** Until part B's canvas-size editor exists, a scene shown by one output alone through its untouched default slice, whose canvas equals that output's effective resolution, follows a resolution or rotation change (`OutputManager::followOutputSizes`), so single-projector shows behave as before. A new scene takes the first output's effective resolution.
- **UI (minimal).** Scenes panel: per scene, a combo of the outputs showing it (each entry toggles). Inspector (selected output): a Scene combo. The editor canvas aspect is now the scene's canvas.
- **Not in part A (QM-121):** slice editor with the Tab/arrow/Shift/Alt corner keys, auto-blend, blend test pattern, canvas size editing, crossfades (§5 fades), TD-12 presentation (§6). Calibration still assumes an output shows its canvas unwarped at 1:1; with offset or warped slices, calibration points are in projector pixels while the canvas is not.

## Amendment 2 (review fixes to part A, 2026-09-27, QM-120)
A fresh-context review found routing bugs that put the wrong picture on a projector during routine operator actions. What changed from Amendment 1:
- **Slices remember their canvas.** An output keeps the canvas size its slices were made for (`OutputWindow::slicesCanvas`) and `showScene` rescales from it, so slices kept through "(no scene)", a Scenes-panel toggle-off or a deleted scene land right on the next scene. Slices of an unknown canvas are replaced by one whole-canvas slice. Deleting a scene takes it off every output showing it. An output that shows no scene is saved with no sceneId and no slices, because its rectangles belong to a canvas the file would not name.
- **Take fallback.** When no output shows the active scene (it was just added, or clicked in the Scenes panel), a take goes to the first enabled output **and every output that shows the same scene as it**, so a blended group still changes together. This replaces "with none, on the first enabled output".
- **Canvas adoption.** Operator routing (Inspector, Scenes panel, takes, and the route a v1/v2 file gets on load) goes through `OutputManager::routeScene`: when the output is the only one showing the scene, through its default slicing, the canvas takes the output's effective (rotated) resolution. The picture is not resampled, and the editor and 3D preview have the projector's aspect. Quads are normalized, so the projector shows the same geometry. Groups and edited slices keep their canvas.
- **Migration.** A pre-3.4 scene no output shows gets the first output's effective resolution; only a file without outputs gets 1920x1080. A 3.4 output with a sceneId and no slices gets one whole-canvas slice at load.
- **F11.** Entering and leaving perform mode sets the output to the monitor's mode, then restores it. A lone default canvas still follows, but `followOutputSizes` does not report a change that comes with an output going fullscreen or back, so perform mode no longer marks an unchanged show modified.
- **Black level.** The lift outside the blend zones is `L + (1 − L)·(canvas over what is already in the frame)`: colour `L + (1 − L)·c.rgb`, alpha `1 − (1 − L)(1 − c.a)`. Alpha 1 had replaced the 3D objects under a lifted slice with black. `CanvasSliceRenderTest.ShaderMatchesSliceBlendGainOnEveryEdge` ties the shader to `sliceBlendGain`/`outsideBlendZones` per pixel (worst 1 level).
- **Canvas target.** The canvas is colour only; 3D objects render per output, so a depth buffer went unused (about 16 MB per scene at 1080p). `MAX_CANVAS_DIMENSION` is 8192, the texture size every GL 3.3 (Direct3D 10 class) GPU supports, applied on create and load (was 16384, 2 GiB with depth). A canvas above `GL_MAX_TEXTURE_SIZE`, or one the GPU runs out of memory for, is logged once and not retried until its size changes. Its outputs then show their 3D objects but no quads.
- **Rescale.** `rescaleSlices` scales edges in double and snaps an edge within 1/1000 px of a whole pixel onto it, so a whole-canvas slice stays exactly whole (float had given 1920.0001 for 800 → 1920) and keeps the exact-copy path.
- Gate a (`RenderOnceReadbackTest`) and gate b are unchanged and pass.
