# Current Status

**Last updated:** 2026-06-17 (end of Session 17)
**Phase / focus:** **Multi-projector mapping — DONE.** [ADR-039](docs/adr/0039-multi-scene-output-routing.md)
multi-scene output routing is **complete (P1–P5) and app-verified end-to-end**. The AI wedge
(Phase 9) stays software-complete, gated on 🚩 QM-061 (hardware).
**Branch:** `dev` (Sessions 16+17, pushed to `origin/dev` @ `e5c08fa`). **`master` = clean
baseline `fb4c85b`** (pre-Session-16 — `dev` is well ahead).
**Build:** ✅ app builds & launches   **Tests:** ✅ 624/624 ctest

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
**No ADR-039 work remains.** Pick from independent items:
1. **Open the `dev` → `master` PR.** `master` is still at `fb4c85b` (pre-Session-16); `dev` has
   the mapping-editor fixes + the *entire* ADR-039 feature (P1–P5), all verified. Ideally land it
   after the QM-061 hardware test, but a PR could go up now since everything is software-verified.
2. 🚩 **QM-061** — real-device AI test (projector + surface + phone). Hardware-only. Runbook
   @docs/QM-061-hardware-test-runbook.md. On pass, this is the trigger to merge `dev`→`master`.
3. **Optional follow-up:** groups-on-load in `syncProjectToEditor` (see limitation below).
4. **Optional debt:** TD-07 (build exit-code flake), TD-08 (audio snapshot, LOW), TD-09 (dual
   media model + on-quad camera).

## How to use multi-projector today (now fully operable)
**Resources ▸ Scenes** tab: New/rename scenes, click to make one active, assign each to an output
via the dropdown. **Resources ▸ Preview** tab: live thumbnail of each projector's content.
**Resources ▸ Outputs** tab: monitor / resolution / rotation / enable + **Perform Mode (F11)**.
Save/Load (Project tab or Ctrl+S) persists the whole multi-scene + outputs setup as `.qmap` v3
(old v1/v2 files still load).

## Known limitation (pre-existing, NOT a regression)
Quad **groups** round-trip in the `.qmap` file but the load bridge (`syncProjectToEditor`) does
**not** re-apply them to live scenes — the same gap the old flat bridge had. Fixable in a small
follow-up if needed.

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
