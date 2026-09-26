# ADR-052: 3D projection mapping foundation — projector camera, mesh objects, point-pair calibration

**Date:** 2026-09-25
**Status:** Accepted
**Deciders:** Jones

## Context

The owner's next goal is AI-assisted and camera-assisted automatic mapping, including mapping onto 3D objects rather than only flat quads. Today everything is 2D:
- A quad is four corners in an output's normalized [0,1] space (ADR-043/QM-086).
- The AI wedge (ADR-035, QM-055..062) solves a single planar homography per selected quad. It persists nothing and has no projector model (see the research notes in the Session 22 log).

Every tool that maps onto 3D objects models the projector as an inverse camera and solves its lens and pose. That includes MadMapper's Space Scanner, disguise OmniCal, Pixera camera calibration, and TouchDesigner camSchnappr/MAPAMOK. They solve from 2D↔3D point pairs, or from dense structured-light correspondences, and then render the mesh from that viewpoint. None of these pieces exist here. They also change the `.qmap` schema and the render path, which makes this an architecture decision.

## Decision

Build the foundation in four slices (Phase 3D-A). Use no new third-party dependency; OpenCV stays in the Python sidecar for the later structured-light work.

### 1. Projector camera model (`src/scene/ProjectorCamera.hpp`)
- `ProjectorIntrinsics` holds pinhole `fx, fy, cx, cy` in pixels for the output resolution. The image origin is top-left with +y down, following the OpenCV convention. `fromThrowRatio()` gives a usable default.
- `ProjectorPose` is the world→camera transform (`R`, `t`), with the camera looking down +Z, +X right and +Y down (OpenCV convention), so solver output is used as-is.
- `ProjectorCamera` = intrinsics + pose + a `calibrated` flag.
  - `project(world) → pixel` is the reference. The OpenGL `viewMatrix()` / `projectionMatrix(near, far)` must reproduce it exactly, and a test enforces that.
- `CalibrationPoint { world (Vec3), pixel (Vec2), enabled }`.
- There is one `ProjectorCamera` and one calibration-point list **per output**, because each physical projector is calibrated separately. They live on `OutputWindow`. World units are the mesh's units.

### 2. Mesh objects (`src/scene/MeshObject.hpp`)
- `MeshData` stores indexed triangles: positions, uvs, normals, indices. The OBJ loader expands faces to triangles, supports `v/vt/vn` combinations and negative indices, and reports errors through `Result`.
- `MeshObject` holds: id, name, source path, `MeshData`, a model→world transform, and a `TextureBinding` (the same media model as quads: `mediaId`, crop, colour), plus visibility and opacity.
- A `Scene` owns a list of `MeshObject`s alongside its quads. The UI calls them "objects".

### 3. Rendering
- Each output renders its scene's mesh objects through its own `ProjectorCamera`, with depth testing, textured by the mesh's own UVs. The `.qmap` stores the mesh path, not the mesh data.
- Quads keep their 2D path and draw on top, unchanged.
- The editor canvas shows the assigned output's projector view (WYSIWYG), using the same matrices with the ADR-041 view transform applied in NDC.
- An uncalibrated projector gets a default camera: throw ratio 1.5, placed so the mesh's bounding box fits the view. An imported mesh is therefore visible immediately.

### 4. Point-pair calibration solver (`src/scene/ProjectorCalibration.*`)
- **Six or more enabled non-coplanar points:** normalised DLT → 3×4 projection matrix → RQ decomposition into K, R and t. Then a Levenberg–Marquardt refinement of `fx, fy, cx, cy` and the pose (Rodrigues vector) to minimise reprojection error.
- **Known intrinsics (the default from throw ratio, or a previous solve):**
  - four or more coplanar or general points → pose only, from a homography (Zhang), then LM refinement;
  - six or more non-coplanar points → pose only, taken from the DLT result.
- Output: the camera plus RMS and per-point reprojection error in pixels. Errors come back as `Result` with actionable messages ("points are coplanar — enter the throw ratio or add depth", "needs ≥ 6 points").
- The linear algebra is small (12×12 at most), so it lives in a local header: Jacobi eigen decomposition, 3×3 RQ, and a dense normal-equation solve. Synthetic-camera tests with noise and outliers prove it.

### 5. Persistence (`.qmap` v3.2, additive per ADR-032/046)
- Each scene gets `"objects": [{id, name, path (relative like sources), transform[16], texture:{…same as quads}, visible, opacity}]`.
- Each output gets `"projector": {intrinsics:{fx,fy,cx,cy,width,height}, pose:{rotation[9],translation[3]}, calibrated, points:[{world[3], pixel[2], enabled}]}`.
- Older builds ignore both fields. A missing mesh file keeps the object with a warning and draws it as empty.

### 6. The calibration UX (slice D, after the above)
- A "Calibrate projector" mode on an output. The user picks a 3D point on the mesh by clicking the model in an orbit-view panel, and places its 2D pixel on the canvas.
- 2D points are nudged with the same Tab/arrow/Shift/Alt keyboard model as quad vertices (`input/VertexKeyboard.hpp`). This is the product's keyboard-precision differentiator, applied to 3D calibration.
- It re-solves live after every change and shows the RMS error.

### Later phases (separate ADRs)
- **3D-B, camera-assisted:** Gray-code structured light in the sidecar (OpenCV `structured_light`) produces dense camera↔projector correspondences. With a ChArUco camera calibration, these become calibration points automatically, or a point cloud.
- **3D-C, AI proposals:** SAM 2 masks and Depth Anything V2 (Small/Base, Apache-2.0) *suggest* surfaces, masks and quads. Geometry stays classical. Metric3D is not shipped until its licence is confirmed.
- **3D-D:** multi-projector blending on one mesh, then occlusion.

## Consequences
- ✅ This is the standard, proven model: calibrate once per projector, then any mesh or content maps correctly from that projector's viewpoint. Flat quads are untouched.
- ✅ No new dependency. The solver and loader are small and unit-tested, and OpenCV stays sidecar-only.
- ✅ The keyboard-precise calibration UX differentiates the product and reuses the success-metric input model.
- ❌ Lens distortion is not modelled in v1. Most projectors are close to pinhole, and radial terms can be added later as an additive field.
- ❌ An OBJ-only import for v1. glTF comes later, probably via cgltf, which needs its own ADR.
- ❌ One projector per mesh is blended naively (no inter-projector blend or occlusion) until 3D-D.

## Alternatives considered
1. **OpenCV in-process via vcpkg** (`solvePnP`, `calibrateCamera`). Rejected for now: a large build and runtime dependency for about 300 lines of well-understood maths. It can be revisited with its own ADR if structured-light decoding moves in-process.
2. **Extend the homography/quad model to curved surfaces with dense mesh warps.** Rejected: manual and view-dependent, and it cannot reproject content correctly onto real 3D geometry.
3. **Monocular depth as the geometry.** Rejected: centimetre-level errors with no metric guarantee. It is a proposal tool only (Later phases above).

## Amendment (QM-093..095 integration, 2026-09-26)
- **On-disk layouts.** An object's `transform[16]` is column-major (glm order). A projector's `pose.rotation[9]` is row-major (OpenCV order).
- **UV convention.** Every frame is stored top row first, so texture v = 0 is the image's top: stills (stb and the WIC TIFF path), video, camera and procedural sources alike. GUI QA on 2026-09-26 found stills upside down on quads while video was upright, because the image loader flipped stills to bottom-row-first; keeping OBJ v as authored hid this on objects but would have shown video upside down there. The loader no longer flips, and the OBJ loader stores v as 1 - v, so OBJ-authored textures and video both show upright on objects. Meshes without UVs get planar UVs from the XY bounds, with world +Y at the image top. A crop's "Top" inset (v = 0 side) now trims the visual top of a still, as it already did for video.
- **Auto lens policy (QM-099).** Auto solves the lens only from 8 or more points, and keeps it only if it is plausible (throw ratio 0.3–5, near-square pixels, principal point near the image). Otherwise it keeps the known lens and solves the pose, with a warning. An explicit lens solve reports an implausible lens as an Err. GUI QA showed that 6 cube corners let one nudge swing the lens to a throw ratio of 0.6, with a principal point 9000 px off.
- **Robust solve.** When the worst reprojection error exceeds 3 px, the solver isolates a single mis-clicked pair and re-fits with a Cauchy loss. It drops nothing, and `perPointError` flags the pair. Clean data never reaches this path. Only one bad pair is handled.
- **Mesh change detection** in the renderer's GPU-mesh cache uses storage pointer, size and endpoint checks, so an in-place edit of interior vertices would not be seen. Nothing edits meshes in place today. A revision counter on `MeshObject` is recorded as TD-15, to add before any mesh-editing feature.
