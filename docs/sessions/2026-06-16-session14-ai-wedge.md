# Session 14 — 2026-06-16 — AI Pre-Show wedge built end-to-end (QM-055→060 + 062)

**Duration:** ~2h25m (17:30–19:55 UTC)
**Outcome:** Phase 9 (AI Pre-Show Assist) is **software-complete**. Started with QM-055 done but
uncommitted; ended with the entire wedge — Surface-Snap Assist + One-Flash auto-warp + anamorphic —
built and watched working in the real app. Only the hardware-gated **QM-061** remains.

## Tasks completed (7)

### QM-055 — committed + merged
Split the Session-13 work into a docs/ADR/research commit + a code commit, merged to `master`.

### QM-056 — AI sidecar skeleton + IPC contract (ADR-035)
- **Python** `tools/ai-sidecar/sidecar.py`: stdlib-only loopback HTTP server, versioned v1
  `/health` + stub `/surface`. 6 GPU-free pytest. Never linked into `QUADmapper.exe`.
- **C++** `src/ai/SidecarClient` (pimpl): WinHTTP client (Windows system lib — **no vcpkg dep**) +
  spawn the sidecar with `CreateProcess` assigned to a **Job Object (KILL_ON_JOB_CLOSE)** so no
  child outlives the app. Attach-or-launch; every path degrades gracefully (never throws).
- **App**: "AI" menu — Enable AI Assist launches on demand; disabled "Sidecar not running" when
  absent. Contract test (C++, skip-if-Python-absent) verifies launch→health→/surface→teardown.
- **Decision:** WinHTTP + Job Object for the transport (the user chose "safe and dependable").

### QM-057 — Metric3D surface service
- `surface.py`: image → **Metric3D metric depth** (call the package's internal
  `predict_depth_normal()` for raw metric depth — the public `__call__` returns colormapped) →
  metric point cloud → **Open3D `segment_plane`** → largest planar component → 4 corners + confidence.
- **Confidence** = planar-quality × **verticality gate** (`1−|normal_y|`; floor/ceiling → ~0) ×
  ambiguity gate. On the 3 spike scenes (RTX 3080): screen **0.58** (encloses), facade **0.59**,
  gallery floor-trap **0.00** (low, not confidently wrong — the spike's failure case fixed).
- ~350ms steady GPU (first call ~7–8s model load). 5 skip-if-model-absent pytest.
- **Authorized + installed:** ran the local ComfyUI Metric3D model (execute code + `torch.load`
  weights) and `pip install`ed open3d/addict/yapf into Python 3.11.

### QM-058 — Detect Surface UI (Phase 1 ships)
- `Application::aiDetectSurface()`: capture a frame (live camera, or `QM_AI_TEST_IMAGE`) →
  `/surface` → apply the proposed corners to the selected quad (confident ≥0.35) or prompt
  "tap the surface". Geometry-only; refine with the existing Tab-cycle + arrow-nudge UX.
- Watched: screen → "surface applied (0.63)", quad snaps to a perspective trapezoid + F11 projects;
  gallery → "low confidence (0.00) — tap the surface", quad unchanged.

### QM-059 — One-Flash pattern emit + capture
- `resources/patterns/charuco.png` + `.json` (7×4 ChArUco, gen by `gen_pattern.py`, **3-channel**
  so it renders white-on-black — a grayscale PNG loads as `GL_RED` → red-on-black).
- `OutputManager::renderPatternToOutputs()` flashes a fullscreen identity quad on all outputs
  (additive; scene path untouched). AI ▸ One-Flash ▸ Emit / Capture / Clear.
- Watched: ChArUco fullscreen white-on-black on the projector; capture → PNG on disk (720×1280×3).

### QM-060 — One-Flash homography solve
- `oneflash.py`: detect the ChArUco in the **reference** + the **photo**, match by marker **id** →
  `findHomography` (projector-norm ↔ photo px); target surface (QM-057 or passed) → `H⁻¹` →
  projector-norm corners. cv2.aruco only (no torch) → GPU-free.
- `POST /oneflash` route; `SidecarClient::requestOneFlash` (shared `parseCornersResponse`); app
  AI ▸ One-Flash ▸ **Solve & Apply**.
- **Validated WITHOUT hardware:** synthetic known-homography test warps the board by a known H,
  asserts the solve recovers corners to **<0.02** (conf 0.99, 0.13px RMS). In-app Solve & Apply
  watched (conf 0.99).

### QM-062 — anamorphic / audience-perspective warp
- Reuses the One-Flash H: map a centered **audience** rectangle back through `H⁻¹` so content reads
  frontal from the captured viewpoint. `oneFlashSolveAndApply` (surface target) + `oneFlashAnamorphic`
  (audience target) share `runOneFlash()`. Anamorphic round-trip test (<2px).
- **Note:** a projector-off (QM-057-only) anamorphic is geometrically underdetermined (the spike's
  finding); this is the One-Flash-based realization. "Looks correct from the viewpoint" is
  perspective-validated on hardware (with QM-061).

## Verification approach
ImGui menu-driving proved unreliable here (a foreground app steals focus, the window moves between
runs, DPI-scaled fonts), so each feature was watched end-to-end via **guarded headless self-test
hooks** — `QM_AI_SELFTEST` ∈ {1, oneflash, solve, anamorphic} + `QM_AI_TEST_IMAGE`, driven by
`tests/smoke/drive_ai_smoke.ps1 -SelfTestMode <mode>`, asserting from the app log + screenshots.
Off in normal runs (the programmatic smoke mode the smoke README asked for). Gate held all session:
**596/596 ctest, 15/15 sidecar pytest, success-metric smoke green, no regression.**

## Decisions
- WinHTTP + Job Object transport (ADR-035's HTTP choice, native, no vcpkg dep).
- Run the local ComfyUI Metric3D model + install open3d/addict/yapf (the sidecar's out-of-build deps).
- Verticality gate makes "largest plane ≠ intended surface" return low confidence.
- ChArUco written 3-channel.
- Anamorphic uses the One-Flash homography (projector pose observed).
- **New debt TD-10:** sidecar `start()` blocks the UI ~1–3s on enable (pre-show; async deferred).
- **Git workflow:** all session-14 work on `dev` (pushed to `origin/dev`); `master` reset to the
  clean Session-12 baseline `fb4c85b` until a `dev`→`master` PR.

## Files (highlights)
- New: `tools/ai-sidecar/{sidecar,surface,oneflash,gen_pattern,test_sidecar,test_surface,test_oneflash}.py`,
  `tools/ai-sidecar/{README.md,requirements.txt,.gitignore}`, `resources/patterns/{charuco.png,charuco.json}`,
  `src/ai/{SidecarClient.hpp,SidecarClient.cpp,CMakeLists.txt}`, `tests/unit/ai/SidecarClientTest.cpp`,
  `tests/smoke/drive_ai_smoke.ps1`.
- Modified: `src/app/{Application.hpp,Application.cpp}`, `src/output/{OutputManager.hpp,OutputManager.cpp}`,
  `src/ui/panels/{MainMenuBar.hpp,MainMenuBar.cpp}`, root + `src/ai`/`tests/unit` CMake, tracking files.

## Handoff — 🚩 QM-061 (hardware-only, the make-or-break)
Connect a projector as a 2nd display → AI ▸ Enable AI Assist → One-Flash ▸ Emit Pattern → F11 →
photograph surface+pattern with a phone → point `QM_AI_TEST_IMAGE` at the photo → One-Flash ▸
Solve & Apply (or Anamorphic Warp) → verify alignment; document failure modes. Fallback if it fails:
MIT gray-code structured-light behind the same `/oneflash` contract. Then open the `dev`→`master` PR.
