---
name: ai-preshow-initiative
description: "The \"Snap-to-Surface\" AI pre-show auto-warp initiative — its research, the validated wedge, and that it's gated on ADR-035 approval."
metadata: 
  node_type: memory
  type: project
  originSessionId: 031ce117-379a-489f-8f21-7b4795918542
---

2026-06-16 planning thread (Session 13): researched integrating AI into QUADmapper's
**pre-show setup** stage (not live performance). Three cited docs in `docs/research/`:
AI prior-art, a full competitor audit, and a feasibility spike.

**The validated wedge:** auto-warp a quad onto a real surface from a phone photo — the niche
**Lightform vacated in 2022, unfilled since**. Camera auto-warp is otherwise paywalled to the
high end (disguise OmniCal, MadMapper Space Scanner). Mobile "AI mapping" apps are vaporware.

**Spike result (GO):** Depth-Anything-V2 / Metric3D run locally (RTX 3080, torch+CUDA, weights
already on disk under ComfyUI's controlnet_aux) recover a clean plane in ~0.6s. Two findings
reshaped the feature: (1) fully-automatic surface picking is unreliable → **human-confirm is
load-bearing**; (2) projector-off passive warp is geometrically impossible (projector pose
unobserved) → build **One-Flash Auto-Warp** (projector flashes one pattern, single phone photo
sees surface+pattern, homography solves directly — no gray-code sequence).

**Architecture:** [[keep-vs-rebuild-decision]] context. AI enters via an **optional out-of-process
Python sidecar** (localhost HTTP+JSON), never in the 60-FPS path or the C++ build — **ADR-035
(Proposed, pending approval; new-dependency Change Boundary)**.

**Status (Session 14):** ADR-035 **Accepted**. **QM-055 DONE** (capture rail + PNG) — merged to
`master` (`af8f69f`). **QM-056 DONE** (sidecar skeleton + IPC boundary) on branch
`feature/qm-056-ai-sidecar` (not yet merged): `tools/ai-sidecar/` stdlib-only HTTP server (v1
`/health` + stub `/surface`, 6 GPU-free pytest); C++ `src/ai/SidecarClient` — WinHTTP client (no
vcpkg dep) + spawn in a **Job Object (KILL_ON_JOB_CLOSE)** so no orphan; "AI" menu launches it on
demand and disables cleanly when absent. 596/596 ctest, success-metric smoke green. Transport
decided as **WinHTTP + Job Object**. Follow-up debt **TD-10** (async `start()` — it blocks the UI
~1–2.5s on enable). **QM-057 DONE** (real surface service, pure Python): `surface.py` — Metric3D
**metric** depth (fixes the spike's affine bow) + Open3D `segment_plane`; confidence = quality ×
**verticality gate** (floor/ceiling→~0) × ambiguity gate. 3 spike scenes: screen 0.58 (encloses),
facade 0.59, gallery floor-trap **0.00** (low, not confidently wrong). ~350ms steady GPU (first
call ~8s model load). Metric3D from local ComfyUI controlnet_aux (`metric_depth_vit_small_800k.pth`,
env-overridable `QM_M3D_*`); call its internal `predict_depth_normal()` — the public `__call__`
returns colormapped depth, not raw metric. Installed into Py3.11: open3d, addict, yapf. **QM-058 DONE** — **Phase 1 ships**: AI menu "Detect
Surface" → `aiDetectSurface()` (capture via QM-055 cam or `QM_AI_TEST_IMAGE` → `/surface` → apply
proposed corners to the selected quad if conf≥0.35, else "tap the surface"; geometry only, no
`.qmap` change). Watched working end-to-end via `drive_ai_smoke.ps1` + the `QM_AI_SELFTEST`
headless hook (menu-driving is unreliable here — a foreground app steals focus, the window moves,
DPI-scaled fonts): screen → applied (0.63) + F11 projects; gallery → low conf (0.00), not applied.
**QM-059 DONE** (Phase 2 starts): One-Flash pattern emit + capture. ChArUco asset
(`resources/patterns/charuco.png`, gen by `tools/ai-sidecar/gen_pattern.py` — **write it 3-channel
or it loads as GL_RED/red-on-black**); `OutputManager::renderPatternToOutputs()` flashes a
fullscreen identity quad on the projector; AI→One-Flash menu (Emit/Capture/Clear); capture →
camera PNG to disk via shared `captureCameraFrameToPng()`. Verified via `drive_ai_smoke.ps1
-SelfTestMode oneflash`. **QM-060 DONE (software-complete) → Phase 2 is software-complete.**
`oneflash.py`: detect ChArUco in reference + photo, match by id → homography H (projector-norm ↔
photo px), target surface (QM-057 or passed) → H⁻¹ → projector-norm corners; `POST /oneflash`;
`SidecarClient::requestOneFlash`; app AI→One-Flash→**Solve & Apply**. **Validated WITHOUT hardware**
via a synthetic known-homography test (recovers corners <0.02, conf 0.99, 0.13px RMS) +
`QM_AI_SELFTEST=solve` in-app. **Only remaining Phase-2 work is 🚩 QM-061** (real projector + surface
+ phone photo — the make-or-break; can't be done in this environment). **QM-062 DONE** (anamorphic / audience-perspective
warp — AI→One-Flash→Anamorphic Warp): reuses the One-Flash homography, maps a centered audience
rect back through H⁻¹ so content reads frontal from the captured viewpoint (a projector-off
QM-057-only version is geometrically underdetermined per the spike). **ALL software-doable AI tasks
are now complete (QM-055..060 + QM-062).** The ONLY remaining wedge work is 🚩 **QM-061** — the
real projector + surface + phone-photo test, which can't be done in this environment. Self-test env
seams: `QM_AI_SELFTEST` ∈ {1, oneflash, solve, anamorphic}, `QM_AI_TEST_IMAGE`. To run QM-061:
projector as 2nd display → AI▸Enable → One-Flash▸Emit Pattern → F11 → photograph surface+pattern →
point `QM_AI_TEST_IMAGE` at it → Solve & Apply. Roadmap in
`docs/research/ai-preshow-roadmap-2026-06-16.md`. See [[quadmapper-ffmpeg-build]] for capture/PNG quirks.
