# Session 10 — 2026-06-16 — `.qmap` versioning + retire multi-scene track

**Branch:** `refactor/app-decomposition` (PR #1) · **Model:** Opus 4.8
**Outcome:** 2 debt items closed (TD-05, TD-02 step 2) + 1 flaky-test fix. 590/590 tests, success
metric harness-verified. 3 commits pushed.

This dated log is the detailed source of truth on conflict (per CLAUDE.md).

---

## Context at start

Session 9 left the branch green with the success metric working; 4 debt items open
(TD-05, TD-02 step 2, TD-07, TD-08). User ran `/start`, then asked me to recommend and proceed
("what do you recommend as a master computer engineer"), and later "move on" / "Push".

## TD-05 — `.qmap` versioning, migration & save/load de-dup (commit `cc6febd`)

**Problem found (worse than the one-liner):** the serializer had **four read entry points with
three different version behaviors**. The flat `load`/`deserialize` one-way-rejected non-1.x; the
*actual app path* `loadToSceneManager` and `deserializeScenes` **never validated version at all**
(silent half-parse of a future file). The quad-into-scene copy block was duplicated 4×, and each
copy dropped mesh-edit vertex offsets (copied only corners/resolution/mode).

**Decision — [ADR-032](../adr/0032-qmap-migration.md):** one `validateVersion()` policy for all
paths (accept major ≤ 2, migrate 1.x→2.x in memory, reject newer majors with an actionable
message); de-dup; **no on-disk format change**. Approved before coding (Change Boundary).

**Done:** `validateVersion()` + `loadFlatProject` (with a v2-`scenes`→flat flatten branch) +
`loadQuadInto` (whole-geometry copy, fixing the mesh-offset drop). 631/631 tests at that point
(+7 new: future/malformed-version reject, missing-version on scene path, v1→scene migration,
v2→flat flatten, mesh-offset round-trip).

## TD-02 step 2 — retire the dual-track scene state (commit `669a05c`)

**Problem found:** the live editor carried **three** representations — `EditorState::scene` (the
real source of truth, edited + rendered), `m_project` (flat `Project`, what save/load actually
persists), and `m_sceneManager` (**inert** — init + reset only). The whole multi-scene feature
(`SceneManager` +35 tests, `ScenePanel`, `.qmap` v2 `scenes` array, `saveWithScenes`/
`loadToSceneManager`) was built but **never wired** — `ScenePanel::setSceneManager()` is never
called; `InspectorPanel`/`PreviewPanel`/`Renderer::renderScene` carried orphan `SceneManager`
hooks with commented-out params. No user can switch scenes; the app only ever writes flat v1.0.0.

**Decision — [ADR-033](../adr/0033-retire-multi-scene.md):** This is a product fork, so I asked.
User chose **delete the dead multi-scene track** (vs wire it up). Single `Scene` becomes the one
source of truth; the flat reader (incl. ADR-032's v2-flatten) still loads legacy v2 files; git +
that read path allow restoring multi-scene later as a deliberate feature.

**Execution:**
- Batch 1 (delegated to a `cpp-executor` subagent, self-verified with build+ctest): deleted
  `SceneManager.{hpp,cpp}`, `ScenePanel.{hpp,cpp}`, `SceneManagerTest.cpp`; removed the orphan
  `SceneManager` hooks from `Application`, `Renderer`, `InspectorPanel`, `PreviewPanel`; removed
  the scene-based `ProjectSerializer` methods/helpers; updated 3 CMake file lists; removed the 5
  serializer scene tests added under TD-05. Build green; the panels' unreachable (always-null)
  `sceneManager_` paths were stubbed rather than deleted (panels are separately unwired).
- Batch 2 (done directly — the subtle part): extracted render()'s 5 copy-pasted save/load sync
  blocks (save ×3, load ×2) into `syncSceneToProject` / `syncProjectToScene` near the top of
  `Application.cpp`, copying the **whole** geometry + texture binding — fixing a latent bug that
  dropped mesh-edit offsets and flipH/flipV on every save/load.

Net **−1648 lines** across 24 files.

## Flaky test fix (commit `172acec`)

`PerformanceMonitorTest.MinMaxFrameTime` slept 5ms and asserted min < 15ms — but Windows' default
~15.6ms timer granularity makes a 5ms sleep routinely overshoot, so it failed whenever no process
held a high-res timer (it passed earlier in the session only by luck of timer state). Rewrote to
assert the min/max **tracking** relationship (max > 15 from the 20ms frame; min ≤ max) instead of
a sub-granularity threshold. Unrelated to the scene work → its own commit.

## Verification (done = seen working)

- Full build clean (only pre-existing `strncpy` deprecation warnings).
- **590/590 tests pass**, 4 audio-integration skipped (need a real video file).
- Smoke harness (`tests/smoke/drive_smoke.ps1`) drove the real app: Top-Left corner warped to
  exactly 0.500/0.400 in the editor; F11 sent the warped quad fullscreen, rendered clean on black
  with no UI/handles, matching the editor geometry. See `tests/smoke/out/05_projector_output.png`.

## Decisions / ADRs

- [ADR-032](../adr/0032-qmap-migration.md) — `.qmap` versioning & migration (additive, no on-disk
  change).
- [ADR-033](../adr/0033-retire-multi-scene.md) — delete the unwired multi-scene track.

## Lessons

- Investigate before scoping a "mechanical" refactor: TD-02 step 2's "dual track" was really a
  *three-way* split with an entirely unwired subsystem; the right move (delete vs wire) was a
  product decision, not a mechanical one — surfaced it to the user rather than guessing.
- A reproducibly-red "flaky" test is worth a 3-line robustness fix; leaving it pollutes the gate.

## Handoff — next session

1. **TD-02 remaining:** decompose the ~2000-line `render()` into real panel classes.
   `InspectorPanel`/`PreviewPanel` are now unwired shells — wire them (or rebuild) and pull UI
   out of `render()`. Largest remaining maintainability item.
2. **TD-07:** `gtest_discover_tests` 5s timeout makes `cmake --build` return non-zero while tests
   are green. Touches CMake (Change Boundary → needs approval). Fold in any further test-timing
   robustness.
3. **TD-08:** defer audio snapshot reclamation to the control thread (LOW).

All work pushed to `refactor/app-decomposition` (PR #1). Restart package:
[notes/CURRENT_STATUS.md](../../notes/CURRENT_STATUS.md).
