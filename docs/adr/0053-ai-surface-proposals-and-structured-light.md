# ADR-053: AI surface proposals (SAM 2) and camera-assisted mapping (Gray-code structured light)

**Date:** 2026-10-03
**Status:** Accepted (owner, 2026-10-03: "as written")
**Deciders:** Jones

## Context

ADR-052 ("Later phases") named two follow-ups to the AI pre-show wedge (ADR-035, QM-055..062). The owner chose both, with proposals first (2026-10-03):
- **3D-C, AI proposals:** SAM 2 masks and Depth Anything V2 *suggest* surfaces and quads.
- **3D-B, camera-assisted:** Gray-code structured light gives dense camera↔projector correspondences.

What exists today:
- **AI menu actions:**
  - **Detect Surface** sends a camera photo to the sidecar's `/surface`. There, Metric3D estimates depth, Open3D finds the largest plane, and four corners come back. The corners are in *photo* space and are applied as if the camera stood at the projector.
  - **One-Flash Solve** photographs a projected ChArUco board and solves one camera↔projector homography. It maps target corners (the full frame, `/surface`'s, or the anamorphic audience rectangle) into projector space. Planar only.
- **All four actions now run off the main thread** (fix ae8c4a0). A slow model no longer freezes the app.
- **The sidecar runs on the `python` found on PATH.** On this machine that is Python 3.11.9 with torch 2.7.1+cu118 on an RTX 3080 (8 GB). That interpreter has `opencv-python`, `opencv-python-headless` *and* `opencv-contrib-python` installed together. They are mutually exclusive, all three install `cv2`, and which one wins is undefined.

Facts checked for this decision (2026-10-03, primary sources unless marked):

| Component | Licence | Notes |
|---|---|---|
| SAM 2 / SAM 2.1, code and all checkpoints | Apache-2.0 (github.com/facebookresearch/sam2 LICENSE) | `pip install sam2`; needs Python ≥ 3.10 and torch ≥ 2.5.1. SAM 2.1 small 184 MB, tiny 156 MB (measured at setup) |
| Depth Anything V2 **Small** | Apache-2.0 (Hugging Face model card) | ≈ 99 MB; loads through the installed `transformers` (`AutoModelForDepthEstimation`) |
| Depth Anything V2 Base / Large / Giant | **CC-BY-NC-4.0** (Large confirmed on its model card; Base and Giant from secondary sources) | Non-commercial: **never used**. ADR-052 wrongly listed Base as Apache-2.0 |
| Metric3D v2 | Code BSD-2-Clause | The checkpoint in use comes from a ComfyUI custom node (`comfyui_controlnet_aux`). No licence grant for that file was found |
| OpenCV contrib (`structured_light`) | Apache-2.0 | `cv2.structured_light.GrayCodePattern`, in `opencv-contrib-python` 4.x |

## Decision

### 1. One isolated sidecar environment, with pinned, hashed models
- **Own environment:** `tools/ai-sidecar/setup.ps1` creates `tools/ai-sidecar/.venv` (git-ignored) and installs a pinned `requirements.txt`. That file uses `opencv-contrib-python` *instead of* `opencv-python`, so the clash above cannot occur in the sidecar's environment.
- **Choosing the interpreter:** `AiAssist::locateSidecar` prefers `.venv\Scripts\python.exe` when it exists. Otherwise it falls back to `python` on PATH, as today.
- **Global Python untouched:** nothing installs into, or uninstalls from, the user's global Python.
- **Model downloads:** `setup.ps1` downloads the model files from their official URLs into `%LOCALAPPDATA%\QUADmapper\models\` and checks a pinned SHA-256 for each. A mismatch deletes the file and fails loudly. `QM_MODELS_DIR` overrides the location.
- **Nothing in the repo:** no model file is committed and none is linked into `QUADmapper.exe`.
- **When something is missing:** a missing model or package makes only its own endpoint fall back to a graceful stub, as today.
- **Advertising features:** `/health` gains `capabilities: [...]`, so the app can grey out an item whose model is absent.

### 2. 3D-C: "Pick Surface", a click-to-segment proposal
- **New "AI Photo" panel** (ui layer, a `Panel` subclass, ADR-051):
  - It shows the last camera capture.
  - The user clicks the surface they want.
  - The sidecar's new `POST /segment {image_path, point: [x, y] normalized}` runs SAM 2.1 (small by default, `tiny` selectable) and returns the mask outline, four corners and a confidence.
- **Corners:** the four corners are the minimum-area quadrilateral that fits the mask's largest contour (`cv2.approxPolyDP` toward 4 points, falling back to `minAreaRect`).
- **Confirm before applying:** the panel draws the outline and corners over the photo, and nothing changes until the user presses **Apply**. Applying follows the same rules as today's results: the quad is picked at the click, nothing is applied under the perform lock or to a deleted quad, and the result is an ordinary warp that Tab/arrows can refine.
- **Mapping into projector space:** photo corners need a camera→projector mapping. The best available one is used:
  1. a Gray-code map (§3) when one is current;
  2. else a One-Flash homography taken this session (passed as `target_corners` to `/oneflash`, exactly as the anamorphic path does);
  3. else photo space as-is, with a status saying that the camera is assumed to be at the projector.
- **Plane check:** Depth Anything V2 **Small** estimates relative inverse depth (disparity), which is linear in image coordinates (u, v) over a plane. The sidecar fits `disparity ≈ a·u + b·v + c` over the mask. The fit's residual becomes a planarity score that is folded into the confidence, so a curved or broken surface is flagged before it is applied. Relative depth is enough for this test, so no metric depth model is needed.
- **Metric3D removed:** `/surface` (Detect Surface, automatic) keeps its contract but changes its engine. SAM 2's automatic mask generator proposes candidate regions. Depth Anything V2 Small scores them for planarity and verticality, and the best one wins. Metric3D and the Open3D plane fit are deleted from the sidecar, because the checkpoint's licence is unconfirmed. Open3D leaves `requirements.txt`.

### 3. 3D-B: a Gray-code camera→projector map, and assisted calibration
- **Who captures:** the app owns the projector and the camera, so the app drives the capture. The sidecar only decodes.
- **"Scan Output"** (AI menu, per output):
  1. It is refused under the perform lock, under blackout, and while another AI request runs.
  2. The app shows OpenCV's Gray-code sequence fullscreen on the chosen output. The sequence has 2·(⌈log₂ W⌉ + ⌈log₂ H⌉) patterns plus a white and a black frame, about 46 frames at 1920×1080. The exact count is read from the generator, not hard-coded.
  3. For each pattern it waits a settle time (default 250 ms; the camera's latency) and saves one camera frame to a scan folder.
  4. All of this is a state machine ticked once per frame, so the app keeps rendering. Esc cancels.
  5. Expect about 15 s at 1080p.
- **Decoding:** the sidecar's new `POST /graycode {folder, projector: [W, H]}` decodes the captures with `GrayCodePattern` and its white/black shadow masks. It writes a dense camera-pixel → projector-pixel map (`.npz`, beside the captures) and returns its path, coverage and a decode-error estimate.
- **Validity:** the map stays valid while the camera and projector stay put. It lives in the session only, with no `.qmap` field. Changing the output's resolution or monitor invalidates it.
- **What the map enables in v1:**
  - **Pick Surface and Detect Surface** corners go through the map (exact per pixel, no planarity assumption) instead of the One-Flash homography.
  - **Assisted 3D calibration (ADR-052 §6):** in the calibration view, the user may click a 3D point's location in the AI Photo panel instead of placing its projector pixel by hand. The map gives the projector pixel exactly, and the usual Tab/arrow nudge stays available. This turns about 6–12 careful placements into clicks.
- **Deferred to 3D-B2, with its own amendment:** ChArUco camera calibration, then camera–projector stereo to make a point cloud and register the mesh to it (ICP), for fully automatic 3D calibration.

### 4. Unchanged
- No change to the render path, the build, or the `.qmap` format.
- No new C++ dependency. The app saves PNGs (the existing `PngWriter`) and parses JSON (nlohmann), as now.
- The sidecar stays optional, and every feature degrades to a status message without it.
- `SIDECAR_API_VERSION` stays 1: the endpoints are additions. An older sidecar answers 404, which the client reports as "update the AI sidecar".

### 5. Verification (owner's budget)
- **Without a GPU, using pytest on synthetic data:**
  - mask → corners on drawn polygons;
  - the planarity fit on synthetic disparity planes vs curved disparity;
  - Gray-code decoding on patterns warped into "camera" images by a known homography and a known non-planar displacement, with the recovered map within 1 px.
- **Model tests:** skipped when the model is absent.
- **C++:** unit tests for the scan state machine and the result-application rules, without a camera or a sidecar.
- **Owner's hands-on check:** the webcam aimed at the 1080p display or a projector.
- **Real-device sign-off:** folded into 🚩 QM-061.

## Consequences
- ✅ Every model used is Apache-licensed, and Metric3D's licence question goes away with Metric3D.
- ✅ Click-to-segment is the "propose, then the human confirms" step the spike said was essential. It reuses the quad, nudge and lock rules unchanged.
- ✅ The Gray-code map removes the planar limit of One-Flash, and it makes 3D calibration point-and-click without needing a camera calibration yet.
- ✅ The sidecar's own environment fixes the OpenCV package clash without touching the user's Python.
- ❌ About 440 MB of models (SAM 2.1 small + tiny + Depth Anything V2 Small) and a 5.2 GB sidecar venv, and a CUDA-capable torch to get good speed. CPU works but is slower; SAM 2 on CPU is reported at seconds per image, unmeasured here. The async requests (ae8c4a0) keep the app responsive meanwhile.
- ❌ A scan takes about 15 s with the projector showing patterns. It is a pre-show step only, refused under the lock.
- ❌ The map is session-only. Moving the camera or projector means rescanning, which is accepted for v1.

## Alternatives considered
1. **Keep Metric3D.** Rejected: there is no licence grant for the checkpoint in use, and relative depth is enough for the plane test.
2. **Depth Anything V2 Base or Large for better depth.** Rejected: CC-BY-NC-4.0.
3. **Install the new packages into the global Python and uninstall `opencv-python` there.** Rejected: it changes the user's machine outside the project, and ComfyUI and other tools may rely on it.
4. **Run SAM 2 through `transformers` (`Sam2Model`) instead of the `sam2` package.** Possible: transformers 4.57.1 is installed. The implementation may choose either after a short spike. The decision recorded here is the model and its licence, not the loader.
5. **In-process OpenCV in C++ for Gray-code decoding.** Rejected, as in ADR-052 alternative 1: a large build dependency, when the sidecar already has OpenCV.
6. **Fully automatic 3D calibration now (stereo + ICP).** Deferred to 3D-B2: it needs a calibrated camera and a much larger verification surface, and assisted clicks deliver most of the time saving first.

## Amendment 1 (implementation, 2026-10-10)

Sections 1 to 3 are built (commits 62fd0bf, c27fbf9, d973cb5, 6fd682d, 3b77499, 1b5a475). The 3D-B2 work is not. The owner has not yet checked any of it on a real camera.

### What was built

- **§1, environment and models.**
  - `tools/ai-sidecar/setup.ps1` builds `tools/ai-sidecar/.venv` from a pinned `requirements.txt` (torch 2.7.1+cu126, `sam2`, `transformers`, `opencv-contrib-python`).
  - It downloads the models into `%LOCALAPPDATA%\QUADmapper\models` and checks a pinned SHA-256 for each.
  - `AiAssist` runs the sidecar on the venv's `python.exe` when it exists.
  - `/health` lists `capabilities`: `surface`, `oneflash`, `segment`, `depth`, and `graycode`.
- **§2, proposals.**
  - AI > Pick Surface... opens the AI Photo panel. A click calls `POST /segment` (SAM 2.1), and the panel shows the outline, corners, confidence and planarity. Nothing changes until Apply.
  - AI > Detect Surface now runs on SAM 2 automatic masks scored with Depth Anything V2 Small. Metric3D, Open3D and the `QM_M3D_*` variables are gone.
  - Every applied AI warp (Pick Surface, Detect Surface, One-Flash) is one undo step.
- **§3, structured light.**
  - AI > Scan Output shows the Gray-code frames fullscreen on the selected output (else the first enabled one) and saves one camera frame of each. The sidecar decodes them (`/graycode/patterns`, `/graycode`, `/graymap`) into a session-only camera-to-projector map. Esc or Cancel Scan stops it.
  - Calibration's Place From Photo sets the selected point's projector pixel from a click on the AI photo. It is one calibration undo step and is refused under the lock.
- **Off the main thread.** Camera capture and sidecar requests run on a worker. `tick()` applies the result on the main thread and drops it under the perform lock or for a deleted quad.

### Measured

- SAM 2.1 small 184 MB, SAM 2.1 tiny 156 MB, Depth Anything V2 Small 99 MB.
- The sidecar venv is 5.2 GB.
- The first request of a run loaded the models in about 12 s (40 s from a cold disk). `/surface` on a CPU-only machine took about 30 s.
- Because of that, the per-request timeout is 120 s (it was 30 s), and the sidecar loads its models on a background thread at startup (c27fbf9). The timeout only bounds a wait now that requests run off the main thread.

### Mapping order

Pick Surface and Detect Surface map their photo corners in this order, and the status says which one applied:
1. a current Gray-code scan map;
2. else a One-Flash homography taken this session;
3. else photo space, with a status that the camera is assumed to be at the projector.

### Scan details

- The scan goes fullscreen first. It then asks the sidecar for the patterns at the size the output shows there, and decodes at that same size, so windowed and fullscreen sizes never mix.
- A map is keyed on the output's display and that display's mode, not the window's size. Leaving perform mode does not make it stale. A change of display or display mode does.
- A display that stays unresolved after going fullscreen, or a failed patterns request, ends the scan.
- Esc in the editor cancels a scan in any phase. Calibration and slice warp editing cannot start during a scan, and a scan cannot start during them.
- A mapped calibration click is dropped unless the photo pick is still on for the same point.

### Open risks

- **Camera timings are guesses.** The 250 ms settle time per frame is a default, not a measurement. It has not been tuned on a real camera, and a slow webcam could capture a frame before the pattern has changed.
- **Mapping is inexact with slices or rotation.** The map is camera pixel to output pixel. A quad corner on an output that uses slices, or an output rotation, does not land exactly where the map says.
- **Exclusive fullscreen at a non-desktop mode** may leave a map stale. The map is keyed on the display's mode, and the mode a display reports during an exclusive-fullscreen switch is not checked on real hardware.
- **Detect Surface was tuned on three images** (screen, facade, gallery floor), not on field photos.
- No model, pattern or camera test has run on a real projector.

### Owner's hands-on check

Setup: run `tools/ai-sidecar/setup.ps1` once. Aim a webcam at the second display or a projector.
1. AI > Enable AI Assist. Wait for "sidecar connected".
2. Select the output in the Outputs panel. Otherwise the first enabled one is scanned.
3. AI > Scan Output. The output goes fullscreen and shows stripes for about 15 s. Esc stops it.
4. When it ends, the app leaves fullscreen and the status reads "Scan: map ready - the camera saw N% of the output (noise X px)". Check that N is high and the noise low.
5. Open the AI Photo panel (Pick Surface...), take a photo, click the surface, and press Apply. The status should say the corners were applied through the scan.
6. In the calibration view, select a point, press Place From Photo, and click the same spot in the AI Photo panel. The point's pixel is set. Ctrl+Z undoes it.

Then, with and without a scan, check Pick Surface and Detect Surface:
- Detect Surface: the status begins "AI: surface found (confidence X) - " and names the mapping used. A low confidence applies nothing.
- Pick Surface: the outline sits on the clicked surface. After Apply, Tab and the arrows refine the corners, and Ctrl+Z undoes the warp.
