# Current Status

**Last updated:** 2026-06-16 (end of Session 14)
**Phase / focus:** **Phase 9 (AI Pre-Show Assist) is SOFTWARE-COMPLETE.** The whole wedge —
Surface-Snap Assist + One-Flash auto-warp + anamorphic — is built and watched working. The only
remaining AI work is the hardware-gated **🚩 QM-061** (real projector + surface + phone photo).
**Branch:** `dev` (all Session-14 work, pushed to `origin/dev`). **`master` = clean baseline
`fb4c85b`** (Session 12) — main stays clean until a `dev`→`master` PR.
**Build:** ✅ app builds & launches   **Tests:** ✅ 596/596 ctest + 15/15 sidecar pytest
**Smoke:** ✅ green (success metric intact: warp + F11 projector)

## What works right now (grounded, verified this session)
The **core projection-mapping success metric is intact** (add quad → warp via Tab-cycle +
arrow-nudge → F11 to projector) AND the **entire AI pre-show wedge runs end-to-end in the app:**

- **Detect Surface (QM-058):** capture a frame → Metric3D surface detection → the proposed quad is
  applied to the selected quad; low confidence prompts "tap the surface" (gallery floor-trap → 0.00,
  screen → 0.58). Refine with the normal Tab+arrow UX, send F11.
- **One-Flash (QM-059/060):** AI ▸ One-Flash ▸ Emit Pattern flashes a ChArUco fullscreen on the
  projector; Solve & Apply detects the pattern in a photo, solves the projector↔surface homography,
  and warps the quad so content lands on the surface. Synthetic test recovers corners to <0.02
  (conf 0.99).
- **Anamorphic (QM-062):** AI ▸ One-Flash ▸ Anamorphic Warp maps a centered audience rectangle back
  through the homography so content reads frontal from the captured viewpoint.

All AI results are **geometry-only** (ordinary quad corners) — `.qmap`, the C++ build, and the
vcpkg manifest are **untouched** (the Python sidecar is an optional out-of-build component).

## NEXT TASK — resume here: **🚩 QM-061 (hardware-only — the make-or-break gate)**
This is the one thing this environment couldn't close. Phase 2 is not "done" until it passes.
1. Connect a **projector** as a second display, aimed at a real surface.
2. App: select a quad → **AI ▸ Enable AI Assist** → **One-Flash ▸ Emit Pattern** → **F11**.
3. **Photograph** the surface + projected pattern with a phone; copy the photo to the PC.
4. Set `QM_AI_TEST_IMAGE` to that photo → **One-Flash ▸ Solve & Apply** (or **Anamorphic Warp**) →
   verify the projected content lands aligned. Document failure modes (lighting, throw, texture).
5. **Fallback if it fails:** port MIT gray-code structured-light behind the same `/oneflash` contract.

When QM-061 passes / after review, open the **`dev`→`master` PR**
(https://github.com/Fiavaion/QUADmapper/pull/new/dev) to bring the wedge into `master`.

Parallel/alternative track (independent of AI): v1.1 polish — color correction per quad (US-104,
low risk) or soft-edge blending (US-101).

## The AI sidecar (how to run it / where things are)
- `tools/ai-sidecar/` — optional Python service (loopback HTTP+JSON). `sidecar.py` routes
  `/health`, `/surface` (Metric3D), `/oneflash` (ChArUco homography). `surface.py`, `oneflash.py`,
  `gen_pattern.py` (regenerates `resources/patterns/charuco.png`). Deps in `requirements.txt`
  (numpy/opencv/open3d/+ torch with CUDA; Metric3D from the local ComfyUI tree via `QM_M3D_*` env).
- C++ side: `src/ai/SidecarClient` (WinHTTP client + Job-Object process lifecycle). AI menu in
  `MainMenuBar`; flow in `Application::{aiDetectSurface, oneFlashEmitPattern, oneFlashCapturePhoto,
  runOneFlash, oneFlashSolveAndApply, oneFlashAnamorphic}`.
- **Verification seams (off in normal runs):** `QM_AI_SELFTEST` ∈ {1, oneflash, solve, anamorphic}
  drives a headless self-test; `QM_AI_TEST_IMAGE` feeds a fixed photo. Driven by
  `tests/smoke/drive_ai_smoke.ps1 -SelfTestMode <mode>`.

## Open debt (none blocking — see TECHNICAL_DEBT.md)
- **TD-10** — sidecar `start()` blocks the UI ~1–3s on enable (pre-show, button-triggered);
  async worker thread when convenient.
- **TD-09** — converge the two media models + on-quad camera display (success-metric risk; own ADR).
- **TD-07** — build exit-code flake (gtest discovery timeout; CMake → needs approval).
- **TD-08** — defer audio snapshot reclamation to the control thread (LOW).

## Verification gate (non-negotiable)
`cmake --build build/debug` → `ctest --test-dir build/debug` (596/596) →
`tests/smoke/drive_smoke.ps1` (warp + projector). AI: `tools/ai-sidecar` → `python -m pytest`
(15/15, surface tests skip without the model). When media is touched, also the media/image smoke.
Run smoke harnesses by **absolute path** (nested `$PSScriptRoot` mangles).

## Blockers
QM-061 needs physical hardware (projector + surface + phone). Everything else is done & green.

## Reference links
- Decisions: @docs/adr/0035-ai-preshow-sidecar.md · @docs/adr/0034-render-decomposition.md · @docs/adr/0031-keep-vs-rebuild.md
- Roadmap: @docs/research/ai-preshow-roadmap-2026-06-16.md (QM-055..062) · spike @docs/research/snap-to-surface-spike-2026-06-16.md
- Debt: @TECHNICAL_DEBT.md · Success metric: @CLAUDE.md#success-metric
- Tasks: @TASKS.md · Machine state: @SESSION_STATE.json · Latest log: @docs/sessions/2026-06-16-session14-ai-wedge.md

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
