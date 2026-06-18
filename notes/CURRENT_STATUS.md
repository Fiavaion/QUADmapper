# Current Status

**Last updated:** 2026-06-17 (Session 18 checkpoint)
**Phase / focus:** **[ADR-040](docs/adr/0040-shared-media-sources-and-per-quad-crop.md) — one video
→ many cropped quads (box-wrap MVP).** **P1 (per-quad crop, QM-068) DONE + GPU-verified;** P2–P5
(QM-069–072) next. ADR-039 multi-scene routing is complete (P1–P5, app-verified). The AI wedge
(Phase 9) stays software-complete, gated on 🚩 QM-061 (hardware).
**Branch:** `dev` (local @ `44dd898`; **unpushed Session-18 commits ahead of `origin/dev`**).
**`master` = clean baseline `fb4c85b`** — `dev` is well ahead. Working tree clean.
**Build:** ✅ app builds & launches   **Tests:** ✅ 630/630 ctest

## What shipped this session (grounded, app-verified)
- **QM-065 (P3) — Scenes UI.** "Scenes" tab ([ScenePanel](../src/ui/panels/ScenePanel.cpp)):
  add / delete / rename / **switch active** scene + **per-scene output-assignment dropdown**
  (one-scene-per-output enforced). Viewport / Quad List / Inspector follow the active scene via
  the existing `activeScene()`/`quadManager()` accessors. `EditorState` gained scene-list mutators.
- **QM-066 (P4) — Live per-output preview.** "Preview" tab
  ([OutputPreviewPanel](../src/ui/panels/OutputPreviewPanel.cpp)): each output's assigned scene
  rendered to a 320×180 offscreen FBO (new RAII [Framebuffer](../src/renderer/Framebuffer.hpp);
  `OutputManager::updatePreviews()` in the main GL context) and shown as a V-flipped thumbnail
  with a "name → scene" label.
- **QM-067 (P5) — `.qmap` v3 persistence.** Writer emits **v3** (`scenes[]` + `outputs[]`) when
  scenes exist, else legacy **v1.2 flat**; reader accepts v1/v2/v3 (`kMaxSupportedMajor` 2→3).
  `Project` gained `SceneData`/`OutputData` DTOs; the Application `syncEditorToProject`/
  `syncProjectToEditor` bridge round-trips **all** scenes + outputs and **remaps each scene's
  `assignedOutputId`** onto the live output ids on load. 5 migration unit tests.

## NEXT TASK — resume here
**In flight: [ADR-040](docs/adr/0040-shared-media-sources-and-per-quad-crop.md) (Accepted) — one
video → many cropped quads, the box-wrap MVP.** **P1 (crop, QM-068) is DONE + GPU-verified.**
1. **ADR-040 P2 (QM-069) — the next step, the risky one.** Make the renderer resolve a quad's
   texture from `SourceManager` via `TextureBinding::mediaId` (the TD-09 convergence), dedupe
   sources by path, retire the `quadTextures` map. **Behavior-preserving first**, success-metric-
   adjacent — start fresh, likely wants an advisor pass on the convergence approach. Then P3
   (shared/multi-quad video), P4 (cross-scene box-wrap proof), P5 (v3.1 persistence).
2. **Open the `dev` → `master` PR** — still gated on 🚩 QM-061 per the user (hold until it passes).
   `master` = `fb4c85b`; `dev` now also carries ADR-039, the groups/TD-07 fixes, and ADR-040 P1.
3. 🚩 **QM-061** — real-device AI test (projector + surface + phone). Hardware-only. Runbook
   @docs/QM-061-hardware-test-runbook.md. On pass, this is the trigger to merge `dev`→`master`.
4. **Optional debt:** TD-08 (audio snapshot, LOW), TD-09 (folds into ADR-040 P2).
   *(groups-on-load and TD-07 were closed 2026-06-17 — see below.)*

## How to use multi-projector today (now fully operable)
**Resources ▸ Scenes** tab: New/rename scenes, click to make one active, assign each to an output
via the dropdown. **Resources ▸ Preview** tab: live thumbnail of each projector's content.
**Resources ▸ Outputs** tab: monitor / resolution / rotation / enable + **Perform Mode (F11)**.
Save/Load (Project tab or Ctrl+S) persists the whole multi-scene + outputs setup as `.qmap` v3
(old v1/v2 files still load).

## Recently closed follow-ups (2026-06-17, on `dev`)
- **Groups-on-load FIXED** (`d586da6`). `deserializeQuad`/`deserializeGroup` now preserve the
  persisted `id`, and `syncProjectToEditor` re-applies each scene's groups onto the live scene
  (member + nested-parent ids remapped onto the freshly minted quad/group ids). Regression test
  `V3GroupMembershipRoundTrips`. *Runtime note:* the serializer linkage is unit-verified; the
  live-scene re-application is verified by code review + the round-trip test, not a GUI run.
- **TD-07 FIXED** (`4ca12c5`). `gtest_discover_tests DISCOVERY_TIMEOUT 60` ends the spurious
  `cmake --build` non-zero exit (see TECHNICAL_DEBT.md).

## Verification gate (non-negotiable)
`cmake --build build/debug` → `ctest --test-dir build/debug` (624/624) → success-metric warp
(corner drag + Tab/arrow + F11 projector). **Build gotcha:** `cmake --build --target` sometimes
skips the exe relink — `touch` the .cpp (and re-`cmake --preset debug` after shader edits) to
force it. **GUI/mouse/multi-monitor verification:** `tests/smoke/drive_scenes_smoke.ps1`
(synthetic Win32 input + `CopyFromScreen`; **pins the window topmost at (0,0)** so the Claude
desktop app can't steal foreground and coords stay stable).

## Reference links
- Decisions: @docs/adr/0039-multi-scene-output-routing.md · @docs/adr/0038-kantan-style-grid-warp.md
- Tasks: @TASKS.md · Machine state: @SESSION_STATE.json · Latest log:
  @docs/sessions/2026-06-17-session17-multiscene-ui-preview-persist.md
- Success metric: @CLAUDE.md#success-metric · Debt: @TECHNICAL_DEBT.md

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
