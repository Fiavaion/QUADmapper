# Current Status

**Last updated:** 2026-06-17 (end of Session 16)
**Phase / focus:** **Multi-projector mapping.** Mapping-editor UX fixes shipped; **multi-scene
output routing ([ADR-039](docs/adr/0039-multi-scene-output-routing.md)) P1+P2 done and
hardware-verified.** The AI wedge (Phase 9) stays software-complete, gated on 🚩 QM-061.
**Branch:** `dev` (all Session-16 work, pushed to `origin/dev` @ `54dcec0`). **`master` = clean
baseline `fb4c85b`.**
**Build:** ✅ app builds & launches   **Tests:** ✅ 619/619 ctest
**Multi-output:** ✅ verified on real 2 monitors (each projector renders its own scene).

## What shipped this session (grounded, verified)
- **Mapping-editor fixes** — DPI-correct picking (cursor ÷ **window** size, not framebuffer);
  **round** aspect-corrected handles + smaller mesh dots; **cyan mesh-grid wireframe** in edit
  mode; **clean 1×1 default grid** (`MIN_MESH_RESOLUTION=1`); **drag-to-position** division
  insert (arm → press → drag → release commits + auto-disarm, Esc cancels); prominent
  **Reset Transform** button. (Diagnosed by running the app + synthetic Win32 input — the
  reported "can't drag verts" was the armed Click-add mode + blob handles, not a picking bug.)
- **ADR-039** — multi-scene + per-output routing + preview (revives the track ADR-033 retired as
  dead code; that ADR anticipated this). `.qmap` → additive **v3** (reads v1/v2). Phased P1–P5.
- **QM-063 (P1)** — `EditorState` holds a scene **list** and edits the **active** scene via
  `activeScene()`/`quadManager()` accessors (always ≥1 scene). Behaviour-preserving.
- **QM-064 (P2)** — `Scene::assignedOutputId`; `renderToOutputs` renders **each output's assigned
  scene** (was: all quads everywhere); pure unit-tested `OutputManager::sceneForOutput()`; default
  scene → **2nd monitor** at startup. **Verified on 2 monitors** (perform mode: monitor 1 = scene
  1 centred quad, monitor 0 = scene 2 top-left quad).

## NEXT TASK — resume here
**QM-065 — ADR-039 P3: Scenes UI.** The multi-projector engine works but **isn't user-operable
yet** — there's no UI to create a 2nd scene or assign outputs. Build a **Scenes panel**:
add / delete / rename / switch the **active** scene + **assign each scene to an output**; the
viewport, Quad List, and Inspector follow the active scene. Revive `ScenePanel` from `669a05c^`
as a reference, but wire it as the active-scene controller (don't reintroduce the inert stub).
Then **P4** (QM-066: per-output live preview via offscreen FBO → thumbnails; revive
`PreviewPanel` from `669a05c^`) and **P5** (QM-067: `.qmap` v3 scenes+outputs persistence +
v1/v2/v3 migration tests — protected `.qmap` boundary, additive + backward-compatible).

## Still open (independent)
- 🚩 **QM-061** real-device AI test (projector + surface + phone) — runbook
  @docs/QM-061-hardware-test-runbook.md; on pass open the `dev`→`master` PR.
- Optional debt: TD-07 (build exit-code flake), TD-08 (audio snapshot, LOW), TD-09 (dual media
  model + on-quad camera).

## How to use multi-output today (engine works; UI is P3)
Resources ▸ **Outputs** tab already has per-output **Monitor dropdown** / resolution / rotation /
enable + **Perform Mode (F11)**. A default output ("Projector 1") is created at launch on the 2nd
monitor with the default scene assigned. F11 fullscreens each enabled output on its monitor,
rendering its assigned scene. Creating a *second* scene + assigning it needs P3.

## Verification gate (non-negotiable)
`cmake --build build/debug` → `ctest --test-dir build/debug` (619/619) → success-metric warp
(corner drag + Tab/arrow + F11 projector). **Build gotcha:** `cmake --build --target` sometimes
skips the exe relink — `touch` the .cpp (and re-`cmake --preset debug` after shader edits) to
force it; verify by exe timestamp / grepping the binary. GUI/mouse/multi-monitor verification:
synthetic Win32 input + virtual-desktop `CopyFromScreen`, cross-checked with `Logger` output.

## Reference links
- Decisions: @docs/adr/0039-multi-scene-output-routing.md · @docs/adr/0038-kantan-style-grid-warp.md
- Tasks: @TASKS.md · Machine state: @SESSION_STATE.json · Latest log:
  @docs/sessions/2026-06-17-session16-multiscene-routing.md
- Success metric: @CLAUDE.md#success-metric · Debt: @TECHNICAL_DEBT.md

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
