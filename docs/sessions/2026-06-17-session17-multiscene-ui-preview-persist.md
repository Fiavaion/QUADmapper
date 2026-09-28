# Session 17 — 2026-06-17 — ADR-039 multi-scene: UI + preview + persistence (P3–P5)

**Focus:** Finish the multi-projector feature. Sessions 15–16 built the runtime
foundation (P1) and per-output routing (P2, hardware-verified) but it was **not
user-operable** — no UI to create/assign scenes, no live preview, and save/load
only persisted the active scene. This session shipped **P3, P4, P5**, completing
**ADR-039 (P1–P5)**.

**Branch:** `dev`, pushed to `origin/dev` @ `e5c08fa`. `master` untouched (`fb4c85b`).
**Gate:** build green; **624/624 ctest** (+5 new serializer migration tests); every
phase app-verified by running the real app.

## Completed this session

- **QM-065 (P3) — Scenes UI.** A "Scenes" tab in the Resources panel (`ScenePanel`):
  New / Delete (disabled at 1 scene) / double-click rename / context menu; click a
  scene to make it active; per-scene **output-assignment dropdown** (one-scene-per-output
  enforced). Viewport / Quad List / Inspector follow the active scene via the existing
  `activeScene()`/`quadManager()` accessors (no extra wiring). `EditorState` gained thin
  scene-list mutators (`addScene`/`setActiveScene`/`deleteScene`/`assignSceneToOutput`/
  `generateUniqueSceneName`/`canDeleteScene`); switching the active scene clears transient
  vertex-selection state so no stale index points into another scene's QuadManager.
  *Reviewer-mode fix:* deferred the per-row context-menu Delete to after the loop to avoid
  a use-after-free on the `scene` pointer used by the output combo.
  **Verified:** created Scene 2 → auto-active, "(2 scenes)", Delete enabled, default scene
  shows "Projector 1", new scene "(no output)".

- **QM-066 (P4) — Live per-output preview.** New RAII `Framebuffer` (renderer, color-only,
  idempotent resize/destroy — satisfies the no-raw-GL-handles rule).
  `OutputManager::updatePreviews()` renders each output's assigned scene to a 320×180
  offscreen FBO in the **main** GL context (reusing `sceneForOutput` routing + clean
  `renderQuad`), saving/restoring the framebuffer binding + viewport so the frame is
  undisturbed; FBOs cleaned on output removal and in `shutdown()`. `OutputPreviewPanel`
  shows each output's thumbnail (V-flipped) with a "name → scene" label; `Application`
  calls `updatePreviews()` once per frame after the viewport render.
  *Note:* the reference `PreviewPanel` from `669a05c^` was an interactive vector editor
  (overlaps `ViewportEditor`), so P4 shipped a true FBO thumbnail instead of reviving it.
  **Verified:** Preview tab showed "Projector 1 → New Scene" with the default quad rendered
  on black, correctly oriented.

- **QM-067 (P5) — `.qmap` v3 persistence.** *Protected boundary; additive + backward-compatible.*
  - **P5a serializer core** (`3a721d3`): `Project` gained `SceneData`/`OutputData` DTOs.
    Writer emits **v3** (`scenes[]` + `outputs[]`) when the project has scene DTOs, else the
    legacy **v1.2 flat** format (byte-compatible with before). Unified reader yields ≥1 scene:
    v1 flat→1 scene, v2 scenes→list, v3 +outputs; the flat `quads_` view is derived from the
    scenes. `kMaxSupportedMajor` 2→3; a v4 file is still rejected loudly. 5 migration unit tests.
  - **P5b bridge** (`e5c08fa`): `syncEditorToProject`/`syncProjectToEditor` round-trip ALL
    scenes + outputs. Load rebuilds the OutputManager's outputs (for v3 files) and **remaps
    each scene's `assignedOutputId`** onto the freshly created output ids; v1/v2 files keep
    their existing outputs, and a fallback assigns the active scene to the first output so
    older projects still project on F11.
  **Verified end-to-end:** saving the default project wrote a v3 file with `outputs[]` +
  `scenes[]` + a matching `assignedOutputId`; loading a hand-crafted **2-scene** v3 file
  restored the Scenes tab ("Alpha → Beamer", "Beta → (no output)") and the Preview
  ("Beamer → Alpha" rendering the quad).

## Files changed (18 files, +1290 / −110)

- **New:** `src/ui/panels/ScenePanel.{hpp,cpp}`, `src/ui/panels/OutputPreviewPanel.{hpp,cpp}`,
  `src/renderer/Framebuffer.{hpp,cpp}`, `tests/smoke/drive_scenes_smoke.ps1`.
- **Changed:** `src/app/Application.cpp` (Scenes/Preview tabs, `updatePreviews` call,
  rewritten save/load bridge), `src/app/EditorState.hpp` (scene mutators + new panel members),
  `src/output/OutputManager.{hpp,cpp}` (preview FBOs), `src/project/Project.hpp` (DTOs),
  `src/project/ProjectSerializer.cpp` (v3 read/write), `src/renderer/CMakeLists.txt`,
  `src/ui/CMakeLists.txt`, `tests/unit/project/ProjectSerializerTest.cpp` (+5 tests),
  `tests/smoke/.gitignore`, `TASKS.md`.

## Commits (on `dev`, pushed)

- `1fcaf43` feat(ADR-039 P3): Scenes UI — operable multi-scene + per-output routing (QM-065)
- `72adf6e` feat(ADR-039 P4): live per-output preview thumbnails (QM-066)
- `3a721d3` feat(ADR-039 P5a): .qmap v3 serializer core — scenes + outputs (QM-067)
- `e5c08fa` feat(ADR-039 P5b): save/load bridge for all scenes + outputs (QM-067)

## Decisions / notes

- P3 wired `ScenePanel` as the active-scene controller over `EditorState` (NOT the retired
  `SceneManager` stub) — switching the active scene is enough for all panels to follow.
- One-scene-per-output is enforced in the Scenes UI and on load remap, because
  `sceneForOutput` returns the first match (a shared output would be ambiguous).
- P4 renders previews in the main context (not per-output contexts) — simpler, no context
  switching; the per-output `Renderer`s stay for real perform-mode output.
- The `.qmap` writer stays v1.2 for flat-only Projects so existing files/tests are byte-clean;
  v3 only appears once scenes exist.

## Verification method (unchanged, works on this machine)

Synthetic Win32 input + `CopyFromScreen` via `tests/smoke/drive_scenes_smoke.ps1`. Two new
robustness fixes this session: **pin the window topmost at (0,0)** (the Claude desktop app
kept stealing foreground and the window drifted run-to-run, breaking hardcoded click coords),
and `AttachThreadInput` foreground-steal. Coordinates are read from cropped screenshots.

## Known limitation (pre-existing, NOT a regression)

Quad **groups** round-trip in the `.qmap` file (DTO + flat) but the load bridge
(`syncProjectToEditor`) does not re-apply them to live scenes — the same gap the old flat
bridge had. Worth a small follow-up PR if groups-on-load is needed.

## Next session

ADR-039 is complete. Remaining work is independent:
- 🚩 **QM-061** real-device AI test (projector + surface + phone) — hardware-only; runbook
  @docs/QM-061-hardware-test-runbook.md; on pass open the `dev`→`master` PR.
- Optional debt: **TD-07** (build exit-code flake), **TD-08** (audio snapshot, LOW),
  **TD-09** (dual media model + on-quad camera).
- Optional follow-up: groups-on-load in `syncProjectToEditor` (see limitation above).
- Consider opening the `dev` → `master` PR — `dev` has Sessions 16+17 (mapping fixes +
  full ADR-039) and `master` is still at the pre-Session-16 baseline `fb4c85b`.
