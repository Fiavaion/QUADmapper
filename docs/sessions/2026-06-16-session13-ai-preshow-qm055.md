# Session 13 — 2026-06-16 — AI pre-show planning + QM-055 (camera capture rail)

**Focus:** A planning thread on integrating AI into QUADmapper's **pre-show setup** stage,
carried all the way to a verified first implementation. Three cited research docs → ADR-035
(architecture) → phased roadmap (QM-055..062) → a GO feasibility spike → QM-055 built and
hardware-verified.

## What happened (in order)

1. **Scoped the question with the user** (camera+depth+phone sensing; auto-cal / splatting /
   generative; pre-show only; deliverable = cited research). Ran the `deep-research` workflow
   (5 angles, 22 sources, 25 claims verified). The workflow's auto-synthesizer botched its
   structured output, so synthesis was done in the main session from the verified claim set.

2. **Research deliverables** (`docs/research/`):
   - `AI-preshow-mapping-2026-06-16.md` — prior-art landscape + novelty scoring.
   - `competitor-audit-2026-06-16.md` — 17-product market audit + functionality × replication
     matrix. Key finding: the affordable + easy + AI-assisted niche is **empty** (Lightform
     vacated it in 2022; camera auto-warp is paywalled to the high end; mobile "AI" is vaporware).

3. **Feasibility spike (GO)** — `snap-to-surface-spike-2026-06-16.md` + `snap-spike-evidence/`:
   ran Depth-Anything-V2-Large locally (RTX 3080, torch+CUDA; weights already on disk under
   ComfyUI's controlnet_aux) on 3 generated projection-surface scenes. Inference ~0.6s; planar
   surfaces fit to <0.5% RMS. **Two findings reshaped the feature:** (a) fully-automatic surface
   picking is unreliable (gallery → floor) → **human-confirm is load-bearing**; (b) projector-off
   passive warp is geometrically impossible (projector pose unobserved) → build **One-Flash
   Auto-Warp** (one projected pattern + one phone photo → homography). Verified the spike's PNG
   pipeline ends up readable; the spike is honest about novelty being absence-of-evidence.

4. **ADR-035 (Accepted)** — AI enters via an **optional out-of-process Python sidecar**
   (localhost HTTP+JSON); render path / C++ build / `.qmap` untouched. Approved by user.

5. **Roadmap** — `ai-preshow-roadmap-2026-06-16.md`, QM-055..062, checkable AC each, with
   🚩 QM-061 (real-device test) as the make-or-break gate.

6. **QM-055 implemented and verified** (this is the code work — see below).

## QM-055 — DONE (capture rail + PNG); on-quad display deferred to TD-09

Built in batches, each build→test→verify, all confirmed on the real **"HD User Facing"** webcam:

- **Batch 1 — capture core.** `VideoDecoder::openCamera()` opens a dshow device (FFmpeg
  libavdevice — already in the vcpkg ffmpeg, **no new dependency**; CMake one-liner approved by
  user). Refactored `open()` → shared `setupDecoderFromFormat()` (behavior-preserving). Added
  `isLive()`. Verified via a temporary probe: opened 1280×720/30fps mjpeg, decoded live RGB
  frames; probe reverted.
- **Batch 2 — source.** `CameraSource : TextureSource` (live, non-playable; drains the
  background-decode buffer to the newest frame for low-latency preview) + `CameraDevices`
  DirectShow enumeration (`#pragma comment` system libs — no vcpkg change) +
  `SourceManager::createCameraSource()`. New `test_source` group (2 tests; real-hardware,
  skip-if-absent so CI stays green).
- **Batch 3a — AC2 capture-to-PNG.** The default vcpkg ffmpeg has **no PNG encoder**, so wrote a
  **dependency-free** `writeRgbPng()` (stored-DEFLATE PNG). **Verified readable by OpenCV**
  (the sidecar's libpng) — shape (720,1280,3), 2.76 MB.
- **Verification gate:** full build clean; **592/592 ctest** (+2); **media smoke green** — the
  green-crystals video still decodes → warped quad → projector (the `open()` refactor preserved
  the success metric). Reviewer-mode pass: refactor correct, COM init/uninit balanced, RAII
  clean, no scope creep.

**Deferred (user decision):** AC1's "camera displayed *on a quad*/projector" needs the renderer
wired to `SourceManager` — the unconverged **TD-09** dual-model. Capture+PNG (what the AI roadmap
needs) is complete; on-quad display rides on the deliberate TD-09 convergence. Recorded in
`TASKS.md`, `TECHNICAL_DEBT.md` (TD-09 row), `CURRENT_STATUS.md`.

## Decisions
- **ADR-035 Accepted:** AI via optional out-of-process Python sidecar (not pybind, not C++ ONNX,
  not cloud).
- **"Snap-to-Surface" reframed** to One-Flash Auto-Warp + human-confirm (spike findings).
- **libavdevice enabled** for webcam capture (already present; no new dependency).
- **On-quad camera display deferred to TD-09** (success-metric-adjacent; do deliberately).

## Files
- **New code:** `src/video/PngWriter.{hpp,cpp}`, `src/source/CameraSource.{hpp,cpp}`,
  `src/source/CameraDevices.{hpp,cpp}`, `tests/unit/source/CameraSourceTest.cpp`.
- **Modified code:** `src/video/VideoDecoder.{cpp,hpp}` (openCamera + refactor),
  `src/source/SourceManager.{cpp,hpp}` (factory), `CMakeLists.txt`, `src/video/CMakeLists.txt`,
  `src/source/CMakeLists.txt`, `tests/unit/CMakeLists.txt`.
- **Docs/planning:** `docs/adr/0035-ai-preshow-sidecar.md`, `docs/research/*` (4 docs + evidence).
- **Tracking:** `TASKS.md`, `CURRENT_STATUS.md`, `TECHNICAL_DEBT.md`.

## Handoff
- **⚠️ Work is uncommitted** on branch `feature/qm-055-camera-input` (incidental `imgui.ini`
  reverted). Verified and clean, but not committed (commit-only-when-asked). First action next
  session: commit (suggest splitting planning-docs+ADR from the QM-055 code), then merge or stack.
- **Next task: QM-056** — AI sidecar skeleton + IPC contract (`tools/ai-sidecar/`). It consumes
  the PNG `CameraSource::captureCurrentFrameToPng()` now produces. Models are on disk.
- **Make-or-break later:** 🚩 QM-061 real-device test (real projector + phone) — the wedge's
  novelty is a hypothesis until watched working on hardware (the spike environment couldn't).
