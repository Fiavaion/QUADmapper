# QUADmapper Project History

This document maintains a chronological record of all development activities, decisions, and changes made to the QUADmapper project.

---

## Session Log

### Session 18 - 2026-06-17 - follow-up fixes + ADR-040 (shared media + crop) scoped & P1 shipped

**Focus:** Two cleanups the user requested (groups-on-load, the TD-07 build flake), then a
hypothetical that became MVP work: wrap one video around a box's faces across two projectors.
Scoped it as **[ADR-040](docs/adr/0040-shared-media-sources-and-per-quad-crop.md)** (Accepted) and
shipped **P1 (per-quad crop)**, GPU-verified. 8 commits on `dev` (`4ca12c5`..`f2f253e`),
**unpushed**; `master` untouched (`fb4c85b`). **630/630 ctest**; build green.

**Completed this session:**
- **Groups-on-load FIXED** (`d586da6`). Two breaks: `deserializeQuad`/`deserializeGroup` ignored
  the persisted `id` (so loaded quads got fresh ids while group members kept saved ids — linkage
  severed), and `syncProjectToEditor` never rebuilt groups. Fixed both: defensive id-preserving
  parse (`parseIdOrNull`; malformed → fresh, older files still load) + a remapping group rebuild
  on load (members + nested parents). Test `V3GroupMembershipRoundTrips`.
- **TD-07 RESOLVED** (`4ca12c5`). `gtest_discover_tests DISCOVERY_TIMEOUT 60` ends the spurious
  `cmake --build` non-zero exit; discovery mode unchanged so `ctest --test-dir` is unaffected.
  (First tried `PRE_TEST` mode — reverted, it broke the standard no-`-C` ctest invocation.)
- **ADR-040 scoped & Accepted** (`f41a317`). One decoded video → many quads, each cropped, from a
  single decode → frame-sync is structural, decode cost is flat. Converges TD-09 (renderer reads
  `SourceManager` via the dormant `TextureBinding::mediaId`). `.qmap` → additive v3.1. Phased
  QM-068–072.
- **QM-068 (ADR-040 P1) — per-quad crop DONE** (`f5c2920`, `b3912fc`). Normalized
  `TextureBinding::cropRect`; `quad.frag` composes crop outside the UV transform (identity = no-op);
  Renderer sets `u_CropOffset`/`u_CropSize`; collapsed Inspector "Crop (source region)" with
  X/Width/Y/Height + vertical-quarter quick-sets (1/2/3/4/Full); `.qmap` writes `crop` only when
  non-identity (pre-crop files byte-stable). **Verified at the GPU level:** new `test_render_gl`
  target renders a 2×1 red|green source through the real shader into an FBO and reads back the
  centre — left-half crop → red, right-half/right-quarter → green (skip-if-no-context).

**Files changed:** 14 files, +602 / −28. New: `docs/adr/0040-*.md`, `tests/unit/renderer/
CropRenderTest.cpp`. See
[docs/sessions/2026-06-17-session18-fixes-and-adr040-crop.md](docs/sessions/2026-06-17-session18-fixes-and-adr040-crop.md).

**Commits (dev, UNPUSHED):** `4ca12c5` TD-07 · `d586da6` groups · `f41a317` ADR-040 scope ·
`f5c2920` P1 crop · `b3912fc` P1 GPU test · plus 3 docs commits (`17563fe`, `44dd898`, `f2f253e`).

**Decisions:** crop is normalized (reuses the UV shader pipeline, identity is byte-clean) and
written conditionally (no version bump, color/soft-edge precedent); the heavy media-model
convergence is P2 (QM-069) and is success-metric-adjacent, so it's deferred to a fresh start with
an advisor pass. Verification depth for P1 was the user's call → GPU readback test (the repo's
first real-GL test), not just data round-trip.

**Handoff:** resume at **ADR-040 P2 (QM-069)** — renderer resolves a quad's texture from
`SourceManager` via `mediaId`, retire `quadTextures`, behavior-preserving first. `dev` is 8 commits
ahead of `origin/dev` and **unpushed** (push when ready). Still holding the `dev`→`master` PR until
🚩 QM-061 (hardware) per the user.

### Session 17 - 2026-06-17 - ADR-039 multi-scene: UI + preview + persistence (P3–P5) — COMPLETE

**Focus:** Finish the multi-projector feature. Sessions 15–16 built the runtime foundation (P1)
and per-output routing (P2, hardware-verified), but it was **not user-operable**. This session
shipped **P3 + P4 + P5**, completing **ADR-039 (P1–P5)**. 4 commits on `dev` (pushed to
`origin/dev` @ `e5c08fa`); `master` untouched (`fb4c85b`). **624/624 ctest** (+5 new serializer
migration tests); every phase app-verified by running the real app.

**Completed this session:**
- **QM-065 (P3) — Scenes UI.** A "Scenes" tab (`ScenePanel`) over `EditorState`: New/Delete/
  rename/switch-active + per-scene output-assignment dropdown (one-scene-per-output enforced);
  viewport/QuadList/Inspector follow the active scene via existing accessors. `EditorState`
  gained thin scene-list mutators. Reviewer-mode fix: deferred context-menu Delete to avoid a
  use-after-free. Verified: created Scene 2 → auto-active, "(2 scenes)", Delete enabled.
- **QM-066 (P4) — Live per-output preview.** New RAII `Framebuffer`; `OutputManager::
  updatePreviews()` renders each output's assigned scene to a 320×180 offscreen FBO in the main
  GL context (saves/restores framebuffer+viewport); `OutputPreviewPanel` shows V-flipped
  thumbnails with a "name → scene" label. Verified: "Projector 1 → New Scene" with the quad
  rendered on black. (The reference `PreviewPanel` was an interactive editor overlapping
  `ViewportEditor`, so a true FBO thumbnail shipped instead.)
- **QM-067 (P5) — `.qmap` v3 persistence** (protected boundary; additive + backward-compatible).
  Serializer writes v3 (`scenes[]` + `outputs[]`) when scenes exist, else legacy v1.2 flat;
  unified reader yields ≥1 scene for v1/v2/v3; `kMaxSupportedMajor` 2→3 (v4 rejected). `Project`
  gained `SceneData`/`OutputData` DTOs; Application `syncEditorToProject`/`syncProjectToEditor`
  round-trip ALL scenes + outputs (load rebuilds outputs and remaps `assignedOutputId`).
  Verified end-to-end: default save → v3 file with output+scene+assignment; 2-scene v3 load →
  Scenes "Alpha→Beamer"/"Beta→(no output)", Preview "Beamer → Alpha" rendering the quad.

**Files changed:** 18 files, +1290 / −110. New: `ScenePanel.*`, `OutputPreviewPanel.*`,
`Framebuffer.*`, `drive_scenes_smoke.ps1`. See
[docs/sessions/2026-06-17-session17-multiscene-ui-preview-persist.md](docs/sessions/2026-06-17-session17-multiscene-ui-preview-persist.md)
for the detailed log.

**Commits (dev, pushed):** `1fcaf43` P3 · `72adf6e` P4 · `3a721d3` P5a · `e5c08fa` P5b.

**Decisions:** P3 wired the panel as the active-scene controller (not the retired SceneManager);
one-scene-per-output enforced because `sceneForOutput` returns the first match; P4 previews
render in the main context; the writer stays v1.2 for flat-only projects (byte-clean).

**Known limitation (pre-existing):** groups round-trip in the file but the load bridge does not
re-apply them to live scenes (same as the old flat bridge).

**Handoff:** ADR-039 done. Remaining is independent — 🚩 QM-061 hardware AI test, debt
TD-07/08/09, optional groups-on-load follow-up. Consider opening the `dev`→`master` PR
(`master` is still at pre-Session-16 `fb4c85b`; `dev` has all of Sessions 16+17).

---

### Session 16 - 2026-06-17 - mapping-editor fixes + multi-scene output routing (ADR-039 P1+P2)

**Focus:** Field-driven fixes to the on-canvas mapping editor, then the start of the
multi-projector feature — reviving multi-scene (retired in ADR-033 as dead code) as a *wired*
capability so each projector shows its own mapping. 8 commits on `dev` (pushed to `origin/dev`,
`54dcec0`); `master` untouched. 619/619 ctest throughout.

**Completed this session:**
- **Mapping-editor UX fixes** (diagnosed by running the app + driving synthetic Win32 input):
  - **Picking** normalized by **window** size, not framebuffer size (DPI-correct hit-tests).
  - **Round handles** — aspect-corrected handle shader (was 2:1 ellipses); smaller mesh dots so a
    dense grid is readable. Verified on screen.
  - **Mesh-grid wireframe** drawn in edit mode (cyan), so division lines are visible and warp live.
  - **Clean 1×1 default grid** (`MIN_MESH_RESOLUTION=1`) — a new quad has no internal divisions;
    you add them. (Stopgap: control grid still = render mesh until Tier 2; textured warps are
    affine until divisions are added — user-approved tradeoff.)
  - **Drag-to-position division insert** — arm Click-add Column/Row, press in the viewport and
    drag the new line into place; release commits + auto-disarms (Esc cancels). `insertU/VDivision`
    return the inserted index; new `moveU/VDivision` clamps between neighbours.
  - **Prominent "Reset Transform"** button (offset/coverage/scale/rotation/flips).
  - +3 control-grid unit tests; MeshSubdivision validity test updated for MIN=1.
- **ADR-039 written** — multi-scene project model + per-scene output routing + live preview;
  deliberately supersedes ADR-033's removal (which anticipated this revival). `.qmap` → additive v3
  reading v1/v2. Phased P1–P5.
- **QM-063 (P1) DONE** — multi-scene runtime foundation: `EditorState` holds a scene list and edits
  the **active** scene via `activeScene()`/`quadManager()` accessors (lazily guarantees one scene).
  Behaviour-preserving; all editor call sites migrated.
- **QM-064 (P2) DONE** — per-output scene routing: `Scene::assignedOutputId`; `renderToOutputs`
  renders each output's assigned scene (was: all quads to every output); pure unit-tested
  `OutputManager::sceneForOutput()`; startup creates a default output and routes the default scene
  to the **2nd monitor**. **Verified on real 2-monitor hardware** — perform mode showed each
  projector rendering its own scene (centred quad on monitor 1, top-left quad on monitor 0).

**Decisions:** ADR-039 (revive multi-scene as a wired feature; 1 scene → at most 1 output; restore
deleted `SceneManager`/`ScenePanel`/`PreviewPanel`/v2-serializer from `669a05c^` as reference and
wire them properly). Live scenes belong in `EditorState` (runtime truth); `Project` becomes the
serialized DTO (v3, P5). Multi-scene UX clarified by the user: a project holds multiple scenes,
each a separate mapping assigned to one output; editor edits one active scene at a time.

**Files (highlights):** `src/shaders/handle.vert`, `src/renderer/Renderer.{hpp,cpp}` (round
handles + mesh-grid wireframe), `src/ui/panels/ViewportEditor.cpp` + `InspectorPanel.cpp`
(picking, drag-insert, reset), `src/scene/QuadGeometry.{hpp,cpp}` (1×1 default, move/insert),
`src/app/EditorState.hpp` (multi-scene accessor), `src/scene/Scene.hpp` (assignedOutputId),
`src/output/OutputManager.{hpp,cpp}` (per-output routing), `src/app/Application.cpp` (default
output + render call), `docs/adr/0039-*`, tests.

**Handoff → next session resumes at QM-065 (P3: Scenes UI).** The multi-projector engine works
but isn't user-operable yet — there's no UI to create a 2nd scene or assign outputs. P3 adds a
Scenes panel (add/rename/switch active + assign output) with viewport/QuadList/Inspector following
the active scene. Then P4 (per-output live preview; revive `PreviewPanel` from `669a05c^`) and P5
(`.qmap` v3 scenes+outputs persistence + v1/v2/v3 migration tests). Build gotcha noted: `cmake
--build --target` sometimes skips the exe relink — `touch` the .cpp to force it.

### Session 15 - 2026-06-17 - v1.1 polish + Kantan-style grid Tier 1 (2D wedge focus)

**Focus:** Independent-of-AI 2D-wedge work while QM-061 stays hardware-gated: paid down TD-10,
shipped two v1.1 polish features, answered four user feature questions (sizing each honestly),
shelved 3D as out-of-scope, and built **Tier 1 of the Kantan-style non-uniform grid warp** end
to end. 14 commits, all on `dev` (pushed to `origin/dev`); `master` untouched.

**Completed this session:**
- **TD-10 resolved** — `SidecarClient::startAsync()` runs attach/launch/health-poll on a worker
  thread (atomic state + new `Starting`; worker joined in stop()/dtor). AI-enable no longer
  freezes the UI ~1–3s; run-loop posts the connect outcome. Unit test added.
- **US-104** per-quad color correction (ADR-036) — brightness/contrast/gamma/RGB gain in
  `quad.frag`; additive `.qmap` field; Inspector section. Identity = render no-op.
- **US-101** per-quad soft-edge blending (ADR-037) — per-edge feather + gamma; for multi-projector
  overlap. **Visually verified** (feathered projector screenshot).
- **Load-path fix** — `syncProjectToScene` now carries colorCorrection/softEdge (they round-tripped
  in JSON but were dropped on the app's scene rebuild).
- **#2 UV test-grid** — Blender-style colored UV grid (`src/renderer/UvTestGrid.*`, `test_renderer`)
  + "Apply UV Test Grid" button + "UV grid on new quads" toggle. **Visually verified** warped on
  quad+projector.
- **#3 Kantan grid — ADR-038 accepted; Tier 1 COMPLETE** (Kantan Linear parity):
  - batch 1: non-uniform control-grid model (uCoords/vCoords replace uniform N×N), behavior-
    preserving; insert/remove division logic. Success-metric edit path is index-based → unchanged.
  - batch 2: `.qmap` persistence + migration (coords written only when non-uniform; old files
    migrate losslessly); writer `1.1.0 → 1.2.0`.
  - batch 3a: Inspector — Even Grid slider + Insert-pos + Add/Del Column/Row.
  - batch 3b: viewport **click-on-edge insert** via `QuadGeometry::inverseBilinear` (unit-tested
    round-trips); arming suppresses drag-starts.
- **#4 3D scan / model-UV — researched then SHELVED** (deliberate 2D focus; roadmap/spike preserved).
- **QM-061** turnkey hardware runbook + ready `dev`→`master` PR-prep doc.

**Decisions:** ADR-036 (color correction), ADR-037 (soft-edge), ADR-038 (Kantan grid — Tiers 1+2
then field-test then Tier 3; **one mesh model**, uniform migrated away + "even grid" action). 3D
shelved. `.qmap` bumped 1.0.0 → 1.2.0 (two additive MINOR steps, major unchanged, backward+forward
compatible).

**Verification:** build clean; **615/615 ctest** (+19 this session); success-metric smoke green
throughout; US-101 / #2 / Tier-1 grid each visually confirmed via temporary env demos (scaffolding
removed). The live *positioned mouse-click* grid gesture isn't automatable by the keyboard-only
smoke harness — its pieces (inverse-bilinear round-trip, insert logic, division rendering) are each
verified; needs a ~10s manual click-confirm.

**Files (highlights):** `src/scene/QuadGeometry.*` (control-grid model + inverseBilinear),
`src/scene/MeshSubdivision.cpp`, `src/renderer/Renderer.cpp` + `UvTestGrid.*`,
`src/ui/panels/{InspectorPanel,ViewportEditor}.cpp`, `src/ai/SidecarClient.*`,
`src/app/{Application,EditorState}.*`, `src/project/ProjectSerializer.cpp`, `src/shaders/quad.frag`,
ADR-036/037/038, `docs/QM-061-*`, `docs/research/3d-...-roadmap`.

**Handoff / next:** **Tier 2 — Bezier handles + decoupled fine tessellation** (smooth curves, full
Kantan parity), then field-test → **Tier 3** (local subdivision + T-junction stitching). Also still
open: **🚩 QM-061** hardware test → then the `dev`→`master` PR (body ready). Optional debt: TD-07/08/09.

---

### Session 14 - 2026-06-16 - AI Pre-Show wedge built end-to-end (QM-055→060 + 062)

**Focus:** Took Phase 9 from "QM-055 done, uncommitted" to **the entire AI pre-show initiative
software-complete** — Surface-Snap Assist + One-Flash auto-warp + anamorphic — in 7 tasks, each
watched working in the real app. Only the hardware-gated QM-061 remains.

**Completed this session:**
- **QM-055** committed (2-commit split: docs/ADR + code) and merged to master.
- **QM-056** — AI sidecar skeleton + IPC contract: Python stdlib HTTP server (`tools/ai-sidecar/`,
  versioned `/health` + stub `/surface`); C++ `src/ai/SidecarClient` — WinHTTP client (no vcpkg
  dep) + spawn in a **Job Object (KILL_ON_JOB_CLOSE)** so no orphan; "AI" menu launches on demand,
  disables cleanly when absent. Contract test (C++) + 6 GPU-free pytest.
- **QM-057** — Metric3D surface service: metric depth (fixes the spike's affine bow) + Open3D
  `segment_plane` → 4 corners + confidence; **verticality gate** drops the gallery floor-trap to
  ~0. On the 3 spike scenes: screen 0.58 (encloses), facade 0.59, gallery 0.00. ~350ms steady GPU.
- **QM-058** — Detect Surface UI: capture → sidecar → apply proposed quad to the selected quad,
  refine with Tab+arrow; low-confidence → "tap the surface". **Phase 1 ships.**
- **QM-059** — One-Flash pattern emit + capture: ChArUco asset (`resources/patterns/`),
  `OutputManager::renderPatternToOutputs()` flashes a fullscreen identity quad on the projector;
  capture → camera PNG to disk via shared `captureCameraFrameToPng()`.
- **QM-060** — One-Flash homography solve: `POST /oneflash` detects ChArUco in reference + photo,
  matches by id → H (projector-norm ↔ photo px); surface target → H⁻¹ → projector corners.
  **Validated without hardware** by a synthetic known-homography test (corners <0.02, conf 0.99,
  0.13px RMS) + in-app Solve & Apply.
- **QM-062** — anamorphic / audience-perspective warp: reuse the One-Flash H, map a centered
  audience rect back so content reads frontal from the captured viewpoint.

**Verification:** every step watched working in the real app via guarded headless self-test hooks
(`QM_AI_SELFTEST` ∈ {1, oneflash, solve, anamorphic}, `QM_AI_TEST_IMAGE`), since ImGui menu-driving
was unreliable (a foreground app steals focus; the window moves; DPI-scaled fonts). Gate held all
session: **596/596 ctest**, **15/15 sidecar pytest**, success-metric smoke green, no regression.

**Decisions:** WinHTTP + Job Object for the C++↔sidecar transport (ADR-035's HTTP choice);
authorized running the local ComfyUI Metric3D model + installing open3d/addict/yapf into Python
3.11; ChArUco written 3-channel (a grayscale PNG loads as GL_RED → red-on-black); the faithful
anamorphic uses the One-Flash homography (a projector-off version is geometrically
underdetermined — the spike's finding). New debt **TD-10** (sidecar `start()` blocks the UI
~1–3s; async deferred).

**Git/merges (tidied):** all 7 tasks merged; **all session work now lives on `dev`** (pushed to
`origin/dev`, PR-able) and **`master` reset to the clean Session-12 baseline** (`fb4c85b`) — main
stays clean until a dev→master PR. Stale merged branches deleted; tmp scaffolding removed.

**🚩 Handoff — the only remaining AI work is QM-061 (hardware-only):** connect a projector as a 2nd
display → AI▸Enable → One-Flash▸Emit Pattern → F11 → photograph surface+pattern with a phone →
point `QM_AI_TEST_IMAGE` at it → Solve & Apply / Anamorphic Warp → check alignment; document
failure modes. Fallback if it fails: port MIT gray-code structured-light behind the same
`/oneflash` contract. Detailed log: `docs/sessions/2026-06-16-session14-ai-wedge.md`.

---

### Session 13 - 2026-06-16 - AI pre-show planning + QM-055 (camera capture rail)

**Focus:** A planning thread on integrating AI into the **pre-show setup** stage, carried to a
verified first implementation. Detailed log: `docs/sessions/2026-06-16-session13-ai-preshow-qm055.md`.

**Planning (Phase 9 opened — "AI Pre-Show Assist"):**
- Ran `deep-research`; synthesized 3 cited docs in `docs/research/` (AI prior-art landscape;
  17-product competitor audit; feasibility spike). Key market finding: the affordable + easy +
  AI-assisted niche is **empty** — Lightform vacated it in 2022, camera auto-warp is paywalled to
  the high end, mobile "AI mapping" is vaporware.
- **Feasibility spike (GO):** Depth-Anything-V2 runs locally (RTX 3080; weights already on disk),
  ~0.6s/photo, planes fit <0.5% RMS. Reshaped the feature: fully-automatic surface picking is
  unreliable → **human-confirm is load-bearing**; projector-off passive warp is geometrically
  impossible → build **One-Flash Auto-Warp** (one pattern + one phone photo).
- **ADR-035 Accepted:** AI via an **optional out-of-process Python sidecar** (render path / C++
  build / `.qmap` untouched). Roadmap QM-055..062 with 🚩 QM-061 (real-device test) as the gate.

**Completed — QM-055 (capture rail + PNG), verified on the real webcam:**
- `VideoDecoder::openCamera()` (FFmpeg dshow; `libavdevice` linked — already in vcpkg, no new dep;
  `open()` refactored into shared `setupDecoderFromFormat()`, behavior-preserving).
- `CameraSource : TextureSource` + `CameraDevices` DirectShow enumeration +
  `SourceManager::createCameraSource`; `test_source` group (2 tests, real-hardware, skip-if-absent).
- **AC2:** `writeRgbPng()` — dependency-free PNG encoder (default vcpkg ffmpeg has no PNG encoder);
  **verified readable by OpenCV** (720×1280×3).
- **Gate:** 592/592 ctest (+2); media smoke green (video→quad→projector success metric intact).
- **Deferred (user decision):** on-quad camera *display* → TD-09 (renderer not wired to
  `SourceManager`). Capture+PNG — the AI-roadmap-critical part — is complete.

**Decisions:** ADR-035 (Python sidecar); One-Flash reframe; enable libavdevice (no new dep);
on-quad display deferred to TD-09.

**⚠️ Handoff:** work is **uncommitted** on `feature/qm-055-camera-input` (verified, clean). Next:
commit (split docs/ADR from code), then **QM-056** (AI sidecar skeleton, consumes the captured PNG).

---

### Session 12 - 2026-06-16 - TD-02 batch 4 (last): ViewportEditor — render() decomposition COMPLETE

**Focus:** The final, success-metric-critical batch of TD-02 (QM-054). Plan-first, then
execute directly in the main Opus session per the resume brief.

**Completed (QM-054 → DONE):**
- **Batch 4** (`f2dd41f`): extracted the pre-`ImGui::NewFrame()` viewport-editing block
  (quad render to the main framebuffer + corner-pin/mesh-edit interaction + the
  keyboard-capture guard, render() ~567–873) into a new **non-`Panel`** class
  `quadmapper::ViewportEditor` (`src/ui/panels/ViewportEditor.{hpp,cpp}`), called via
  `es.viewportEditor.render(ctx)` from the same pre-NewFrame spot. The moved body is
  **byte-for-byte identical**, wrapped in same-named aliases (`m_renderer`/`m_window`/
  `m_project`/`quadManager`/corner-mesh-snap-video fields) — LESSON-ARCH-006.
- Removed the now-unused corner/mesh/snap + `videoAssignedQuadId` aliases from `render()`
  (kept the video-pump aliases still in use).
- Deleted the dead `PreviewPanel.{hpp,cpp}` source-arch shell + its CMake entry; added
  `ViewportEditor` to `quadmapper_ui`.

**Deviation from ADR-0034's table** (which named `PreviewPanel` as the home): per the
session-11 resume brief, a non-`Panel` `ViewportEditor` + delete-the-shell — consistent with
the batch 3b `VideoPlayerPanel` deviation. Noted in the commit.

**Verification (the gate, all green):** build clean (only pre-existing `strncpy` C4996
warnings, none in the new file) → `ctest` 590/590 (4 audio integration skipped) →
`drive_smoke.ps1` 5 screenshots incl. `05_projector_output.png` (visibly warped quad
fullscreen on monitor 2; `02_*.png` shows the keyboard-nudged corner with the green selected
handle) → **fresh-context Opus verifier: APPROVE**, all 7 success-metric criteria, byte-for-byte
block comparison (306 lines, zero diff).

**Net:** `Application.cpp` 2411 → ~960 lines; `render()` body ~1910 → ~400. **TD-02 closed**
(moved to Resolved in TECHNICAL_DEBT.md).

**Merged PR #1 → `master`** (merge commit `5ef8809`): the entire ADR-031 keep-and-refactor
(TD-01/03/04/05/06 + all TD-02 steps) is now on `master`; the refactor phase is complete.

**Media workflow verified end-to-end (image + video).** The original smoke harness only warped
a white quad, so "apply image/video and perform" was unverified. Drove the real UI with assets
from `D:\testing Videos`: `greenCrystals_15Sec_4x3.mp4` (FFmpeg) and `knowledge-graph.png`
(1800×1200) each load → assign → warp → F11, rendering on the warped quad in the editor **and**
fullscreen on the projector (monitor 2). Added two reusable harnesses
(`tests/smoke/drive_media_smoke.ps1`, `drive_image_smoke.ps1`; commit `6caf8dc`). The core
projection-mapping workflow (add quad → warp → apply media → perform to projector) is confirmed
fully functional.

**Handoff:** refactor done + merged; start v1.1 from `master`. Recommended next: Color
correction per quad (US-104) with an ADR + acceptance criteria. Open debt (none blocking):
TD-07, TD-08, TD-09. Detailed log: `docs/sessions/2026-06-16-td02-batch4-merge-media-verify.md`.

---

### Session 11 - 2026-06-16 - TD-02: decompose render() into panels (5 of 6 batches)

**Focus:** The last open ADR-031 item — TD-02, decomposing the ~1900-line `render()`
god-function into real panel classes. User asked to scope it, then to apply the model-switcher
rubric and "work optimally." Result: an ADR + contract + a sequenced, model-tiered batch plan,
executed batch-by-batch with per-batch verification.

**Decisions:**
- [ADR-034](docs/adr/0034-render-decomposition.md): decompose over the **live `EditorState`
  model** (behavior-preserving) rather than finishing the half-done `SourceManager` migration the
  unwired `InspectorPanel`/`PreviewPanel` shells assume. The migration is logged separately as
  **TD-09**. Key finding: the vertex-editing block runs **before `ImGui::NewFrame()`** and renders
  via OpenGL, so it cannot become a docked `Panel` — it gets a non-`Panel` `ViewportEditor`.
- Contract: a per-frame `EditorContext` bundle (`EditorState&` + manager pointers) passed to each
  panel's `render(ctx)`; panels are value members of `EditorState`; ImGui window names preserved
  (imgui.ini layout keys).
- Model schedule (modelswitcher): Opus for the contract design + batch-3 review + batch 4;
  Sonnet (via `ui-specialist`) for the mechanical extractions. Fable confirmed unavailable → Opus floor.
- Batch 3b deviation: kept the Video Player as its own `VideoPlayerPanel` window instead of
  "folding" it into the Inspector (the plan's wording) — folding would orphan the
  `showVideoPlayer` toggle / window layout key; behavior-preservation wins.

**Completed (QM-054, in progress — 5 of 6 batches):**
- Batch 0: `src/app/EditorContext.hpp` + extracted New/Save/Load into
  `Application::{newProject,saveProject,loadProject}` (de-duped the 3× inline save/load).
- Batch 1: `MainMenuBar` (`708d267`).
- Batch 2: `QuadListPanel` (`fb23972`).
- Batch 3: `InspectorPanel` rebuilt on the live model = context-sensitive Properties
  (Quad/Output/Media) (`6d522af`) — Opus diff-review confirmed faithful.
- Batch 3b: `VideoPlayerPanel` (`3a13950`).
- Checkpoint: restart package + tracking updated, pushed (`e5cb792`).

**Verified working:** every batch was build-clean + **590/590 ctest** + smoke-harness green
(the full success metric incl. `tests/smoke/out/05_projector_output.png`). `Application.cpp`
2411→1271 lines; `render()` body ~1910→~710.

**Shipped:** commits `708d267`, `fb23972`, `6d522af`, `3a13950`, `e5cb792` (+ batch-0/ADR
commits) pushed to `refactor/app-decomposition` (PR #1).

**Handoff / next steps:** **Batch 4 (last; success-metric critical)** — extract the
pre-`NewFrame()` viewport editing (quad render + corner/mesh warp + keyboard-capture guard,
`render()` ~564–867) into a non-`Panel` `ViewportEditor`; delete the dead `PreviewPanel` shell.
Recommended in a **fresh context**, **Opus + high in the main session** (not Sonnet) + a
fresh-context verifier subagent vs the success metric. Full brief in
[notes/CURRENT_STATUS.md](notes/CURRENT_STATUS.md); contract in
[docs/adr/0034-render-decomposition.md](docs/adr/0034-render-decomposition.md). Then TD-07
(CMake, needs approval), TD-08 (LOW), TD-09 (source-arch convergence).

---

### Session 10 - 2026-06-16 - Debt cleanup: .qmap versioning + retire multi-scene track

**Focus:** Two ADR-031 debt items — `.qmap` versioning/migration (TD-05) and "one scene of
truth" (TD-02 step 2). User delegated task selection ("what do you recommend as a master
computer engineer"); both picked on data-integrity / dead-code grounds.

**Completed:**
- QM-052: TD-05 — unified `.qmap` version policy across all 4 read paths (accept major ≤ 2,
  reject newer with an actionable message; the scene paths previously ignored version
  entirely). v1.x→single-scene and v2.x→flat migration; de-duped the copy-pasted save/load
  into shared helpers; fixed a silent mesh-offset drop on scene load. [ADR-032](docs/adr/0032-qmap-migration.md).
- QM-053: TD-02 step 2 — retired the unwired multi-scene track. Deleted `SceneManager`
  (+35 tests), `ScenePanel`, the scene-based `ProjectSerializer` methods, the orphan
  `SceneManager` hooks in Inspector/Preview/Renderer, and `Application::m_sceneManager`.
  `EditorState::scene` is now the single source of truth. Reconciled render()'s 5 hand-rolled
  `Scene`↔`Project` save/load sites into two shared whole-geometry/texture helpers
  (`syncSceneToProject`/`syncProjectToScene`) — fixing a latent mesh-offset + flipH/V drop on
  save/load. Net −1648 lines. [ADR-033](docs/adr/0033-retire-multi-scene.md).
- Fixed the pre-existing flaky `PerformanceMonitorTest.MinMaxFrameTime` (asserted a sub-
  granularity threshold; broke under Windows' default ~15.6ms timer). Now asserts min/max
  tracking instead.

**Key decisions:**
- [ADR-032](docs/adr/0032-qmap-migration.md): additive, backward-compatible `.qmap` versioning;
  no on-disk format change (writer stays at the format the app already emits).
- [ADR-033](docs/adr/0033-retire-multi-scene.md): delete the non-functional multi-scene
  subsystem rather than wire it up; git + the preserved v2 read path allow restoring it later
  as a deliberate feature.

**Verified working:** full build clean; **590/590 tests** (4 audio integration skipped — need a
real video file); success metric harness-verified end-to-end (vertex warped to 0.500/0.400;
F11 → warped quad fullscreen-clean on the projector, `tests/smoke/out/05_projector_output.png`).

**Shipped:** 3 commits (`cc6febd`, `669a05c`, `172acec`) pushed to `refactor/app-decomposition`
(PR #1).

**Handoff / next steps:** TD-02 remaining — decompose the ~2000-line `render()` into real panels
(`InspectorPanel`/`PreviewPanel` are now unwired shells). Then TD-07 (build exit-code flake —
CMake, needs approval) and TD-08 (audio snapshot reclamation, LOW). See
[notes/CURRENT_STATUS.md](notes/CURRENT_STATUS.md) and [TECHNICAL_DEBT.md](TECHNICAL_DEBT.md).

---

### Session 9 - 2026-06-16 - ADR-031 Refactor: success metric working + critical fixes

**Focus:** Audit-driven keep-and-refactor (ADR-031). Got the full success metric working
end-to-end and resolved all CRITICAL/HIGH audit items.

**Completed:**
- QM-045: ADR-031 5-specialist audit → refactor-in-place; UI smoke harness (`tests/smoke/`)
- QM-046: TD-02 step 1 — render() statics → owned `EditorState`
- QM-047: TD-01 — projector output renders the warped scene (success metric complete)
- QM-048: TD-04 — atomic cross-thread video decoder state (UB fix)
- QM-049: TD-03 — lock-free + alloc-free audio RT callback
- QM-050: TD-06 — deleted dead code
- QM-051: fixed two full-build regressions (AudioBuffer capacity contract; stale test signature)

**Key decisions:**
- [ADR-031](docs/adr/0031-keep-vs-rebuild.md): refactor-in-place, not rebuild (rot localized to
  `Application::render()`; foundations sound). Accepted.
- Verification gate = `tests/smoke/drive_smoke.ps1` (drives the real app; reads screenshots).

**Verified working:** add quad → Tab-cycle vertex → arrow-nudge (Shift 10×) → F11 → warped
quad fullscreen on the projector. 624/624 tests on a full build.

**Lessons:** LESSON-BUILD-007 (full-build before ctest — partial builds run stale test exes,
hid two regressions); LESSON-TEST-004 (GLFW maps keys by scancode, not VK); LESSON-ARCH-006
(de-static a god-function via owned struct + same-named aliases).

**Shipped:** branch `refactor/app-decomposition` pushed; PR #1 opened against master.

**Handoff / next steps:** TD-05 (.qmap migration + de-dup save/load), TD-02 step 2 (real
UIManager/panel dispatch + retire dual-track `m_sceneManager`/static-`Scene`), TD-07 (build
exit-code flake — CMake, needs approval), TD-08 (audio snapshot reclamation, LOW). See
[notes/CURRENT_STATUS.md](notes/CURRENT_STATUS.md) and [TECHNICAL_DEBT.md](TECHNICAL_DEBT.md).

---

### Session 5 - 2025-12-19 - Phase 3 Complete

**Focus:** FFmpeg build verification and video player UI integration

**Completed:**
- QM-014: FFmpeg video decoder - BUILD VERIFIED
- QM-015: Video texture binding - BUILD VERIFIED
- QM-016: Playback controls - UI INTEGRATED
- QM-017: Speed control - UI INTEGRATED
- QM-018: Loop modes - UI INTEGRATED
- QM-019: Timeline lock system - BUILD VERIFIED

**Technical Achievements:**
- FFmpeg 7.1.2 successfully installed via vcpkg
- All video components compile without errors
- Fixed LOG macro warnings (variadic macro support)
- Integrated VideoPlayer into Application.cpp
- Added Video Player panel with full controls:
  - Play/Pause/Stop buttons
  - Skip forward/backward (5s)
  - Progress slider with seek
  - Speed control (0.1x - 4.0x)
  - Loop mode selector (Once/Loop/Ping-Pong)
  - Video info display
  - Assign video to quad functionality
- Added Texture::setGLTextureId for external texture support
- 203/203 tests still passing

**Decisions Made:**
- **ADR-013:** Texture class supports external GL texture IDs (non-owning mode)

**Files Created/Modified:**
- `src/core/Logger.hpp` - Variadic LOG macros
- `src/renderer/Texture.hpp/.cpp` - External texture ID support
- `src/app/Application.cpp` - Video player integration and UI panel
- `src/video/VideoDecoder.cpp` - Removed unused variable warning

**Handoff Notes:**
- **Phase 3 COMPLETE** - All 6/6 tasks done
- Application builds and runs with video support
- Ready for Phase 4, 5, or 6 (can run in parallel)
- Next suggested: QM-020 (Output window system) or QM-024 (Scene data structure)

---

### Session 4 - 2025-12-19 - Phase 2 Completion + Phase 3 Code Complete

**Focus:** Multi-quad support, mesh editing, groups, z-order, UV controls, then video playback

**Part 1: Phase 2 Completed**
- QM-008: Mesh subdivision system (formalized)
- QM-009: Mesh vertex editing mode
- QM-010: Multiple quads with selection system
- QM-011: Quad groups
- QM-012: Z-order management
- QM-013: UV offset/scale controls

**Part 2: Phase 3 Code Complete (Build Pending)**
- QM-014: FFmpeg video decoder - CODE COMPLETE
- QM-015: Video texture binding (FrameBuffer) - CODE COMPLETE
- QM-016: Playback controls - CODE COMPLETE
- QM-017: Speed control - CODE COMPLETE
- QM-018: Loop modes - CODE COMPLETE
- QM-019: Timeline lock system - CODE COMPLETE

**Technical Achievements:**
- QuadManager class with full lifecycle and multi-selection
- QuadGroup with member ID list and visibility/lock propagation
- Mesh vertex editing with draggable handles and bilinear reset
- Z-order operations (up/down/top/bottom) with keyboard shortcuts
- UV transform support (offset, scale, rotation)
- 39 new QuadGroup tests, 203/203 total tests passing
- 6 parallel sub-agents completed all Phase 2 tasks
- VideoDecoder with FFmpeg integration (pimpl pattern)
- VideoPlayer with full play/pause/stop/seek/speed/loop controls
- FrameBuffer thread-safe ring buffer for frame staging
- TimelineSyncManager for synchronized video restart
- FFmpeg added to vcpkg.json (building in background)

**Decisions Made:**
- **ADR-008:** QuadManager handles multi-selection and z-order operations
- **ADR-009:** QuadGroup uses member ID list (not ownership)
- **ADR-010:** Mesh vertex editing via draggable handles with bilinear reset
- **ADR-011:** VideoDecoder uses pimpl pattern for FFmpeg isolation
- **ADR-012:** Background decoding thread with ring buffer

**Files Created/Modified:**
- `src/scene/QuadManager.hpp/.cpp` - Quad collection management
- `src/scene/QuadGroup.hpp/.cpp` - Group with member management
- `src/scene/Scene.hpp/.cpp` - Group management API
- `src/renderer/Renderer.hpp/.cpp` - Mesh handle rendering
- `src/shaders/handle.vert/.frag` - Handle circle shaders
- `src/project/ProjectSerializer.cpp` - Group serialization
- `src/app/Application.cpp` - Major UI updates
- `tests/unit/scene/QuadGroupTest.cpp` - 39 group tests
- `src/video/VideoDecoder.hpp/.cpp` - FFmpeg video decoding
- `src/video/VideoPlayer.hpp/.cpp` - High-level player controls
- `src/video/FrameBuffer.hpp/.cpp` - Thread-safe frame ring buffer
- `src/video/VideoPlaybackState.hpp/.cpp` - Playback state struct
- `src/video/TimelineSyncManager.hpp/.cpp` - Synchronized playback
- `src/video/CMakeLists.txt` - FFmpeg detection and linking
- `vcpkg.json` - Added FFmpeg dependency

**Handoff Notes:**
- **Phase 2 COMPLETE** - All 6/6 tasks done
- **Phase 3 CODE COMPLETE** - All 6/6 tasks implemented
- FFmpeg vcpkg build in progress (15-30 min first time, then cached)
- Next session: run `cmake --preset debug` to complete configuration
- Then build and verify video components compile
- Finally integrate VideoPlayer into Application.cpp

---

### Session 3 - 2025-12-19 - Phase 1 Completion

**Focus:** Project save/load implementation, completing Phase 1

**Completed:**
- QM-007: Implement minimal project save/load (.qmap JSON format)

**Technical Achievements:**
- ProjectSerializer with full quad geometry serialization
- JSON format with version compatibility checking
- File menu with New/Save/Load and Ctrl+N/S/O shortcuts
- Project panel with file path input and status display
- Fixed and updated test suite (95/95 tests passing)

**Decisions Made:**
- **ADR-007:** Project stores quads directly (Scene integration deferred to Phase 2)

**Files Created/Modified:**
- `src/project/Project.hpp` - Added quad and texture path storage
- `src/project/ProjectSerializer.cpp` - Full quad serialization implementation
- `src/app/Application.cpp` - Added File menu and Project panel
- `tests/unit/project/ProjectTest.cpp` - Complete rewrite with serialization tests
- `tests/unit/scene/SceneTest.cpp` - Updated to match Scene API
- `tests/unit/scene/QuadGeometryTest.cpp` - Updated to match QuadGeometry API
- `tests/unit/scene/MeshSubdivisionTest.cpp` - Updated to match MeshSubdivision API

**Handoff Notes:**
- **Phase 1 COMPLETE** - All 7/7 tasks done
- Application can save/load projects with quad corners, mesh settings, and texture paths
- 95/95 unit tests passing
- Ready to begin Phase 2: Mesh & Multi-Quad

---

### Session 2 - 2025-12-19 - Phase 1 Core Implementation

**Focus:** GLFW window, OpenGL rendering, quad warping, texture loading, ImGui inspector

**Completed:**
- QM-001: Setup CMake build system (external dependencies downloaded)
- QM-002: Create GLFW window (1920x1080 with vsync)
- QM-003: Initialize OpenGL context (3.3 Core via GLAD)
- QM-004: Implement single quad with 4-corner warping
- QM-005: Add image texture loading (stb_image integration)
- QM-006: Create basic ImGui inspector panel

**Technical Achievements:**
- ShaderProgram class for compiling/linking GLSL shaders
- Texture class with stb_image for PNG/JPG/BMP/TGA loading
- Renderer with mesh subdivision (2-64 resolution)
- Real-time corner warping with bilinear interpolation
- ImGui docking and multi-viewport enabled
- Core tests: 27/27 passing

**Decisions Made:**
- **ADR-004:** Shader sources embedded via CMake configure_file()
- **ADR-005:** Mesh resolution range 2-64 for quad subdivision
- **ADR-006:** Default quad covers 10%-90% of screen space

**Files Created/Modified:**
- `src/app/Application.cpp` - Full GLFW/OpenGL/ImGui initialization and main loop
- `src/renderer/Renderer.cpp` - Quad rendering with mesh subdivision
- `src/renderer/ShaderProgram.cpp` - GLSL shader compilation
- `src/renderer/Texture.cpp` - Image loading with stb_image
- `src/shaders/CMakeLists.txt` - Shader embedding via file(READ)
- `external/imgui/*` - Downloaded Dear ImGui docking branch (12 files)
- `external/glad/*` - Installed GLAD OpenGL 3.3 Core loader
- `external/stb/stb_image.h` - Downloaded stb_image header

**Handoff Notes:**
- Phase 1 is 6/7 complete (QM-007 project save/load remaining)
- Application renders a warpable quad with texture support
- Quad Inspector panel provides corner, mesh, and opacity controls
- Ready to implement project serialization for QM-007

---

### Session 1 - 2025-12-19 - Initial Project Setup

**Focus:** Development infrastructure setup

**Completed:**
- Created complete project directory structure
- Set up CMake build system with vcpkg integration
- Created all component CMakeLists.txt files
- Wrote CLAUDE.md development guide
- Created formal PRD and EDD documents
- Set up session management system
- Configured sub-agent automation infrastructure

**Decisions Made:**
- **ADR-001:** Using hybrid sub-agent strategy (phase-first, component-parallel)
- **ADR-002:** Session management via /start and /end slash commands
- **ADR-003:** Task tracking with QM-XXX ID format in TASKS.md

**Files Created:**
- `CMakeLists.txt` - Root build configuration
- `vcpkg.json` - Dependency manifest
- `CMakePresets.json` - Build presets
- `CLAUDE.md` - Development guide
- `docs/PRD.md` - Product Requirements Document
- `docs/EDD.md` - Engineering Design Document
- `PROJECT_HISTORY.md` - This file
- `TASKS.md` - Task tracking
- `SESSION_STATE.json` - Auto-continue state
- `.claude/commands/*.md` - Slash commands
- All component `CMakeLists.txt` files

**Handoff Notes:**
- Project structure is complete and ready for implementation
- Next step: Begin Phase 1 (Core Foundation) starting with QM-001
- Build system not yet tested (requires vcpkg dependencies)

---

## Major Milestones

| Date | Milestone | Notes |
|------|-----------|-------|
| 2025-11-23 | Specification Complete | QUAD_MAPPER_MVP_SPECIFICATION.md finalized |
| 2025-12-19 | Infrastructure Setup | PRD, EDD, build system, session management |
| 2025-12-19 | Phase 1 Complete | All 7 tasks done: rendering, textures, inspector, save/load |
| 2025-12-19 | Phase 2 Complete | All 6 tasks done: multi-quad, groups, z-order, mesh editing, UV controls |
| 2025-12-19 | Phase 3 Complete | All 6 tasks done: FFmpeg decoder, video player, UI integration |

---

## Architecture Decisions Record (ADR)

### ADR-001: Sub-Agent Strategy
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Need to efficiently parallelize development work across phases
**Decision:** Use hybrid phase-first, component-parallel strategy with automatic spawn triggers
**Consequences:** Enables parallel development of independent components while respecting dependencies

### ADR-002: Session Management
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Need to maintain development continuity across sessions with context limits
**Decision:** Use /start and /end slash commands with three tracking files (PROJECT_HISTORY.md, TASKS.md, SESSION_STATE.json)
**Consequences:** Enables automatic context recovery and seamless session continuation

### ADR-003: Task ID Format
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Need consistent task identification across tracking systems
**Decision:** Use QM-XXX format (e.g., QM-001, QM-042)
**Consequences:** Clear, sequential, project-specific task identification

### ADR-008: QuadManager Selection System
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Need to manage multiple quads with selection and z-order operations
**Decision:** QuadManager handles multi-selection, z-order changes, and lifecycle management
**Consequences:** Clean separation of quad collection management from rendering

### ADR-009: QuadGroup Membership Model
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Need to organize quads into groups without transferring ownership
**Decision:** QuadGroup stores member IDs (not ownership), quads can be in multiple groups
**Consequences:** Flexible grouping without complex ownership hierarchies

### ADR-010: Mesh Vertex Editing
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Need to allow fine-grained mesh deformation beyond corner warping
**Decision:** Draggable vertex handles with per-vertex offsets, bilinear reset option
**Consequences:** Enables detailed surface correction while preserving original geometry

### ADR-011: VideoDecoder Pimpl Pattern
**Date:** 2025-12-19
**Status:** Accepted
**Context:** FFmpeg headers are complex and pollute namespace
**Decision:** Use pimpl pattern to isolate FFmpeg types in implementation file
**Consequences:** Clean public interface, faster compilation, easier to swap backends

### ADR-012: Background Video Decoding
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Video decoding is CPU-intensive and would block main thread
**Decision:** Background thread with ring buffer for frame staging
**Consequences:** Smooth playback, decoupled decode/render timing

### ADR-013: External Texture ID Support
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Video frames use OpenGL textures managed by Application, need to pass to Texture wrapper
**Decision:** Texture class supports setGLTextureId for non-owning external textures
**Consequences:** Video textures can be used with existing quad rendering without duplicating texture management

---
