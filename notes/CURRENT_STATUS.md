# Current Status

**Last updated:** 2026-06-17 (end of Session 15)
**Phase / focus:** **2D-wedge work.** v1.1 polish shipped + **Kantan-style grid (ADR-038) Tier 1
COMPLETE**. The AI pre-show wedge (Phase 9) stays software-complete and gated on the hardware-only
**🚩 QM-061**.
**Branch:** `dev` (all Session-15 work, pushed to `origin/dev`). **`master` = clean baseline
`fb4c85b`** — main stays clean until a `dev`→`master` PR.
**Build:** ✅ app builds & launches   **Tests:** ✅ 615/615 ctest (+ 15/15 sidecar pytest)
**Smoke:** ✅ green (success metric intact: warp + F11 projector). **`.qmap` writer = 1.2.0.**

## What shipped this session (grounded, verified)
- **TD-10 resolved** — `SidecarClient::startAsync()` (worker thread, atomic state + `Starting`);
  AI-enable no longer freezes the UI.
- **US-104 color correction** (ADR-036) — per-quad brightness/contrast/gamma/RGB in `quad.frag` +
  Inspector "Color Correction"; identity = render no-op.
- **US-101 soft-edge blending** (ADR-037) — per-edge feather + gamma; Inspector "Soft Edge
  Blending". Visually verified.
- **#2 UV test-grid** — `src/renderer/UvTestGrid.*` (Blender-style colored grid) + "Apply UV Test
  Grid" button + "UV grid on new quads" toggle. Visually verified.
- **#3 Kantan grid (ADR-038) Tier 1 COMPLETE** — non-uniform control grid (Kantan **Linear**
  parity): model (uCoords/vCoords) + `.qmap` persistence/migration + Inspector add/remove + viewport
  **click-on-edge insert** (`QuadGeometry::inverseBilinear`, unit-tested). Success-metric edit path
  (drag + Tab-cycle + arrow-nudge) unchanged because it's index-based.
- **#4 3D scan / model-UV** — researched then **SHELVED** (deliberate 2D focus; roadmap preserved).
- **Load-path fix** — `syncProjectToScene` now carries colorCorrection/softEdge.

## NEXT TASK — resume here
**Kantan grid Tier 2 — Bezier handles + decoupled fine tessellation** (smooth curves, full Kantan
parity). Then **field-test the workflow**, then **Tier 3** (local cell subdivision + T-junction
stitching — the "beyond Kantan" capability; its real cost is crack-free rendering). Design + phased
plan: @docs/adr/0038-kantan-style-grid-warp.md.

Quick before/alongside: a **~10s manual confirm of the Tier-1 click-on-edge gesture** — the
keyboard-only smoke harness can't drive a positioned viewport click; its pieces (inverse-bilinear
round-trip, insert logic, division rendering) are each verified.

## Still open (independent of the grid)
- **🚩 QM-061** real-device test (projector + surface + phone) — turnkey runbook at
  @docs/QM-061-hardware-test-runbook.md. On pass → open the `dev`→`master` PR (body ready:
  @docs/QM-061-pr-prep.md).
- Optional debt: TD-09 (dual media model + on-quad camera), TD-07 (build exit-code flake, CMake),
  TD-08 (audio snapshot reclamation, LOW). **TD-10 is done.**

## How to try the grid (app launched this session)
Select a quad → Inspector ▸ Geometry: **Click-add Column/Row** then click in the viewport to drop a
division where you click; **Insert pos** + **Add/Del Column/Row** for precise placement; **Even Grid
(N×N)** resets; tick **Edit Mesh Vertices** then drag handles / Tab+arrows to warp. `.qmap` stores
the non-uniform grid (v1.2). Color Correction + Soft Edge sections are below Geometry.

## Verification gate (non-negotiable)
`cmake --build build/debug` → `ctest --test-dir build/debug` (615/615) →
`tests/smoke/drive_smoke.ps1` (warp + projector). Run smoke harnesses by **absolute path**. When
media is touched, also the media/image smoke. AI: `tools/ai-sidecar` `python -m pytest` (15/15).

## Reference links
- Decisions: @docs/adr/0038-kantan-style-grid-warp.md · @docs/adr/0036-per-quad-color-correction.md ·
  @docs/adr/0037-per-quad-soft-edge-blending.md · @docs/adr/0035-ai-preshow-sidecar.md
- Tasks: @TASKS.md · Machine state: @SESSION_STATE.json · Latest log: @docs/sessions/2026-06-17-session15-v11-grid.md
- Debt: @TECHNICAL_DEBT.md · Success metric: @CLAUDE.md#success-metric

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
