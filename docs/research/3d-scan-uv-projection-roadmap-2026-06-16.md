# 3D Object Scan / Model-UV Projection — Research & Roadmap

**Date:** 2026-06-16 (Session 15)
**Status:** **SHELVED (2026-06-17) — deliberate decision to keep focus on the 2D quad-mapping
wedge + QM-061.** Not rejected on technical merit; parked. Revisit by running the Spike-1/2
feasibility (below) if/when 3D projection mapping becomes a priority. No 3D code will be written
until that spike is run and ADR-039 is accepted.
**Owner:** Jones

## The ask (#4)

> "Add the ability to autoscan 3D objects, or use their UV mapping setup to add procedural
> textures."

Two related capabilities, both of which take QUADmapper from a **2D quad** mapper to **3D
projection mapping** (a.k.a. spatial AR / video mapping onto physical 3D objects):

- **(A) Autoscan a 3D object** — reconstruct the physical object's 3D geometry so projections
  wrap correctly around it.
- **(B) Use a 3D model's UV layout** — import a model (the object's digital twin) with its UV
  unwrap and paint/procedurally texture it via those UVs, then project so the texture lands on
  the real object.

This is an **initiative on the scale of the AI pre-show wedge**, not an incremental feature.
This doc frames it; it does not commit to it.

## Why it's major (gap analysis vs. today's architecture)

Everything in QUADmapper is **2D-quad-shaped**:

| Layer | Today | Needed for 3D |
|-------|-------|---------------|
| Scene model | `Quad` = 4 corners + 2D mesh offsets | 3D mesh (vertices/faces/UVs), object transform |
| `.qmap` | quads, 2D corners | 3D meshes or model refs + pose; **major** schema change |
| Renderer | ortho 2D quad shader (`quad.frag`) | 3D camera/projector projection matrix, depth, per-face UV mapping, possibly per-face occlusion |
| Calibration | drag corners / One-Flash homography (a **plane**) | full **projector intrinsics+extrinsics** vs. the object (6-DoF pose), not a single homography |
| AI sidecar | Metric3D single-plane depth | multi-view fusion / mesh reconstruction (A), or model loading + registration (B) |
| UI | 2D canvas warp | 3D viewport, model load, pose/registration tools |

The AI sidecar (Metric3D + Open3D) is a **partial** foundation for (A) — it already produces a
metric point cloud — but single-view depth ≠ a closed 3D object mesh.

## Approaches

### (B) Model-UV projection — *the more tractable half; recommend first*
1. **Import** a textured 3D model (glTF/OBJ) with its UV layout (new dep: a loader, e.g.
   tinygltf/assimp — **ADR + approval**, it's a build-config Change Boundary).
2. **Render** the model in a 3D viewport; apply procedural/file textures through its UVs (reuses
   the existing procedural sources + the new color-correction/soft-edge shader work).
3. **Register** the model to the real object from the projector's viewpoint — the calibration
   problem: solve projector pose so the rendered model overlays the physical object. Could extend
   One-Flash (project markers, solve PnP instead of a homography) or use manual corresponded
   points.
4. **Output** the model rendered from the projector's pose → projection-maps onto the object.

This is "standard" 3D projection mapping (cf. MadMapper 3D, HeavyM, Resolume). Hardest sub-part
is **registration/calibration** (step 3).

### (A) Autoscan — *the harder half; gate behind (B)*
- **Photogrammetry / multi-view:** capture N photos → COLMAP/OpenMVS or a learned MVS → mesh.
  Heavy deps, GPU, minutes-not-seconds; sidecar-shaped like the AI wedge.
- **Structured-light scan:** reuse the projector — project gray-code sequences (the QM-061
  fallback tech!) from one+ viewpoints → per-view depth → fuse (Open3D TSDF) → mesh. Most
  synergistic with existing hardware/code; still a multi-capture, calibrated process.
- **Depth fusion:** if a depth sensor (RealSense/Kinect/LiDAR phone) is available, fuse frames.

All produce a mesh that then feeds the (B) render/registration path — so **(B) is the spine; (A)
is an input to it.**

## Recommended feasibility spike (before any ADR/commitment)

Time-boxed, sidecar/offline where possible, **no changes to the 2D core**:

1. **Spike-1 (model path):** load a glTF cube/known object, render it in an offscreen 3D view,
   texture via its UVs, and **solve projector pose** from ≥6 manual 2D↔3D correspondences (PnP,
   `cv2.solvePnP` in the sidecar). Output the rendered overlay; eyeball alignment on a screen.
   → answers: is registration accurate/usable enough? what's the calibration UX?
2. **Spike-2 (scan path):** drive the existing `renderPatternToOutputs` to emit a gray-code
   sequence, capture per frame, decode → one-view depth → mesh patch in the sidecar.
   → answers: does projector-based structured light give a usable surface on a real object?
3. **Decision:** if Spike-1 alignment is good → write **ADR-039 (3D projection-mapping
   architecture)** with a phased plan (model import → registration → output; scan as a later
   input). If registration is too fiddly → keep QUADmapper 2D-quad-focused and document why.

## Phasing (only if the spike says go)

- **P0** Spike-1/2 (above) — throwaway, sidecar/offline.
- **P1** 3D scene model + glTF import + 3D viewport (new dep ADR). Render model with current
  texturing (incl. color-correction/soft-edge shaders).
- **P2** Projector registration (PnP / extend One-Flash to pose). The make-or-break gate, like
  QM-061 was for the 2D wedge.
- **P3** Project model from projector pose to the real object (the payoff).
- **P4** Autoscan (A) — structured-light first (reuses P2 calibration + gray-code), photogrammetry
  later. Feeds P1's mesh.
- **`.qmap` v2.x** — 3D meshes/model-refs + pose. Major bump with migration (existing 2D quads
  stay valid).

## Risks / open questions
- **Scope:** this is plausibly larger than the entire current app. Confirm appetite before P1.
- **New dependencies:** model loader, possibly MVS/photogrammetry — each an ADR (build boundary).
- **Calibration UX** is the crux; if it's not fast, live-event users won't adopt it (the 2-minute
  success-metric ethos).
- **Does it dilute the product?** QUADmapper's wedge is fast 2D quad mapping + AI pre-show. 3D is
  a different (crowded) market (MadMapper/HeavyM/TouchDesigner). Strategic call, not just technical.

## Decision needed from Jones
Pursue the **Spike-1/2 feasibility** now (time-boxed, no core changes), or shelve #4 and keep
focus on the 2D wedge + QM-061? No 3D code will be written until this is answered.
