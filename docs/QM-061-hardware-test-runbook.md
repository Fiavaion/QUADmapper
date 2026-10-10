# QM-061 — Real-Device Validation Runbook 🚩

**Status:** TODO (hardware-gated). This is the make-or-break gate for the AI pre-show wedge:
Phase 2 (One-Flash auto-warp) is **not "done"** until this passes on real hardware.
Everything upstream (QM-055..060, QM-062) is software-complete and synthetic-validated. The
ADR-053 features (Pick Surface, Scan Output, Place From Photo) are built and share this gate.

**Goal (acceptance criteria):** On a real projector + real surface + real phone photo,
**One-Flash produces a visibly correct warp** — projected content lands aligned on the surface,
watched working — and failure modes are documented. If it fails, port MIT gray-code
structured-light behind the same `/oneflash` sidecar contract (see Fallback).

---

## 0. Hardware & environment checklist

- [ ] **Projector** connected as a **second display** (Windows: Extend, not Duplicate), aimed
      at a real surface (matte wall / screen / flat object). Note throw distance & angle.
- [ ] **Webcam** (the QM-055 capture rail uses the first DirectShow device — "HD User Facing"
      1280×720 was the dev camera) OR a phone you can copy a photo from.
- [ ] **Phone camera** for the single calibration photo (if not using the webcam).
- [ ] Room you can **dim** (ChArUco detection wants decent contrast; avoid washout/glare).
- [ ] Python sidecar set up: run `tools/ai-sidecar/setup.ps1` once. It builds
      `tools/ai-sidecar/.venv` (about 5.2 GB) and downloads SAM 2.1 small and tiny and
      Depth Anything V2 Small (about 440 MB) into `%LOCALAPPDATA%\QUADmapper\models`, checking
      each SHA-256. **One-Flash itself is GPU-free** (`cv2.aruco` only). Detect Surface, Pick
      Surface and Scan Output use the models, and are much faster on a CUDA GPU.
- [ ] Debug build current: `cmake --build build/debug` → `QUADmapper.exe`.

---

## 1. Wire up & enable AI

1. Launch `build/debug/src/app/Debug/QUADmapper.exe`.
2. Add/select a quad (the default "Quad 1" is fine) — this is the quad One-Flash will warp.
3. **AI ▸ Enable AI Assist.** Status shows "connecting…" then "AI assist enabled
   (sidecar connected)" (TD-10 made this non-blocking). If it says unavailable, confirm
   `tools/ai-sidecar/setup.ps1` has run (the app prefers `tools/ai-sidecar/.venv`) and that
   `tools/ai-sidecar/sidecar.py` exists; check the log. The sidecar loads its models at start,
   so the first request can wait for that. Requests have a 120 s ceiling and run off the main
   thread, so the app stays responsive.

## 1b. Detect Surface and Pick Surface (ADR-053)

3a. **AI ▸ Detect Surface.** Photographs the scene and proposes four corners. The status begins
    "AI: surface found (confidence X) - " and names the mapping used. Low confidence applies
    nothing. Record the confidence and whether the quad lands on the surface.
3b. **AI ▸ Pick Surface...** opens the AI Photo panel. Take Photo, click the surface, check the
    outline and corners, then press Apply. Tab and the arrows refine the corners. Ctrl+Z undoes
    the warp.
3c. Both map their photo corners through a current Scan Output map first (section 5b), else a
    One-Flash homography from this session (section 3), else photo space. Photo space assumes
    the camera stands at the projector, so expect a misfit without a scan or a One-Flash.

## 2. Emit the calibration pattern on the projector

4. **AI ▸ One-Flash ▸ Emit Pattern.** The app arms the ChArUco pattern
   (`resources/patterns/charuco.png`, 7×4 DICT_5X5_100, white-on-black).
5. Press **F11** to send fullscreen to the projector output. The projector now shows the
   ChArUco board on the surface. (If the pattern looks red-on-black, the asset regressed to
   1-channel — regenerate 3-channel via `tools/ai-sidecar/gen_pattern.py`.)

## 3. Capture the surface + pattern

6. Photograph the surface **with the pattern projected on it**, framing the whole board with
   some surface margin. Hold steady; avoid motion blur and specular glare.
   - **Phone path:** take the photo, copy it to the PC (e.g. `D:\testing Videos\qm061.jpg`).
   - **Webcam path:** **AI ▸ One-Flash ▸ Capture** writes
     `quadmapper_oneflash_capture.png` (next to the exe / temp) from the first camera.

## 4. Solve & apply the warp

7. Point the solver at your photo: set the env var **before launching**, or relaunch with it:
   ```powershell
   $env:QM_AI_TEST_IMAGE = "D:\testing Videos\qm061.jpg"
   ```
   (`QM_AI_TEST_IMAGE` overrides the live-camera capture in `aiDetectSurface`/One-Flash so a
   fixed phone photo drives the solve — the same seam the smoke harness uses.)
8. **AI ▸ One-Flash ▸ Solve & Apply.** The sidecar detects the ChArUco in both the projector
   reference and the photo, matches by marker id, solves the homography `H`, maps the target
   surface back through `H⁻¹`, and applies the resulting corners to the selected quad.
   - Watch the status for the confidence (inlier ratio + reprojection RMS). Synthetic baseline
     was conf 0.99 / 0.13px RMS; expect lower on real photos — record what you get.
9. Assign real content (image/video) to the quad and **F11** again. **Verify the content lands
   aligned on the surface.** This is the AC — watch it, screenshot it.

## 5. (Optional) Anamorphic check — QM-062

10. **AI ▸ One-Flash ▸ Anamorphic Warp** instead of Solve & Apply maps a centered *audience*
    rectangle through `H⁻¹` so content reads frontal **from the photo's viewpoint**. Stand where
    the photo was taken and confirm the content looks rectilinear/correct from there.

---

## 5b. Scan Output and Place From Photo (ADR-053)

10a. Select the projector output. **AI ▸ Scan Output** shows Gray-code stripes fullscreen for
     about 15 s and photographs each frame. Esc or **Cancel Scan** stops it. Keep the camera
     and projector still.
10b. On success the status reads "Scan: map ready - the camera saw N% of the output (noise X
     px)". Record N and X. A low N means the camera did not see the output.
10c. Repeat Pick Surface or Detect Surface. The status should say the corners were applied
     through the scan.
10d. In the calibration view, select a point, press **Place From Photo**, and click the same
     spot in the AI Photo panel. The point's pixel is set exactly. Ctrl+Z undoes it.
10e. Note whether the 250 ms settle time per frame was enough for the camera. If frames
     looked wrong or the coverage was low, say so.

## 6. Document the result (fill this in during the test)

| Item | Result |
|------|--------|
| Date / tester | |
| Projector (model, native res, throw dist/angle) | |
| Surface (material, size) | |
| Camera used (phone model / webcam) | |
| Lighting (lux / "dim room" / etc.) | |
| ChArUco detected in photo? (markers found / 24) | |
| Solve confidence (inlier ratio, reprojection RMS px) | |
| Content landed aligned? (yes / partial / no) | |
| Anamorphic (QM-062) correct from viewpoint? | |
| Detect Surface confidence / landed on the surface? | |
| Pick Surface landed on the clicked surface? | |
| Scan Output (coverage %, noise px, settle time enough?) | |
| Pick/Detect through the scan: aligned? | |
| Place From Photo: point set where clicked? | |
| Screenshots / photos (paths) | |

**Failure modes observed** (lighting washout, oblique throw, surface texture/specular,
keystone extremes, marker occlusion, focus/blur, rolling-shutter): _____

---

## 7. Fallback if One-Flash fails on real hardware

If single-shot ChArUco detection is unreliable in the field (low contrast, oblique throw,
textured surface), use **Scan Output** (section 5b). It is the Gray-code structured-light
path from ADR-053 §3 and is already built: the app shows the patterns, the sidecar decodes
them to a dense camera-to-projector map. If One-Flash fails and the scan works, record that.
The notes below are the original port plan, kept for reference:

- Emit a **sequence** of gray-code patterns (horizontal + vertical, plus inverses) instead of
  one ChArUco — `OutputManager::renderPatternToOutputs` already draws a fullscreen quad; drive
  it per-frame from the One-Flash flow with a capture between frames.
- Decode the captured stripe sequence in `oneflash.py` to a dense projector↔camera pixel map,
  then fit the same homography (or a finer mesh) → return the **same corner-offset response**
  the C++ `parseCornersResponse` already consumes.
- Keep the synthetic test harness; add a gray-code decode unit test.
- Trade-off: N captures + a few seconds vs. one flash — acceptable for pre-show; far more robust.

---

## 8. After it passes

- Update **QM-061 → DONE** in [TASKS.md](../TASKS.md) with the documented result, clear the
  blocker in [SESSION_STATE.json](../SESSION_STATE.json), and mark Phase 9 fully complete.
- Open the **`dev`→`master` PR** to bring the wedge (and the v1.1 polish) into `master` — see
  [docs/QM-061-pr-prep.md](QM-061-pr-prep.md) for the ready-to-use PR body.
