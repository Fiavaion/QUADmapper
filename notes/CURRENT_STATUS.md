# Current Status

**Last updated:** 2026-06-16 (end of Session 12)
**Phase / focus:** **ADR-031 refactor COMPLETE and MERGED to `master`.** Ready to start **v1.1 features**.
**Branch:** `master` (PR #1 merged, commit `5ef8809`) — clean baseline for new work.
**Build:** ✅ app builds & launches   **Tests:** ✅ 590/590 (`ctest --test-dir build/debug`)   **Smoke:** ✅ green incl. F11 projector + media (image & video)

## What works right now (grounded, verified this session)
The **core projection-mapping workflow is fully functional and watched working** this session:
add a quad → warp it to map a surface (mouse drag *or* precise keyboard: Tab-cycle + arrow-nudge,
Shift ×10 / Alt ×0.1) → apply an **image or video** → perform it fullscreen to a projector (F11).

- **Video:** `greenCrystals_15Sec_4x3.mp4` (768×576, FFmpeg) load → Assign to Selected Quad →
  Play → warp → F11: renders on the warped quad in the editor **and** fullscreen on monitor 2.
- **Image:** `knowledge-graph.png` (1800×1200) via Inspector Texture Path → Load → warp → F11:
  same, on the projector.
- The projector path (`renderToProjectorOutputs()`) feeds outputs the **same** texture map
  (`quadTextures` + video wrapper) the editor viewport uses.

**ADR-031 refactor (TD-01..TD-06 + all of TD-02) is done and merged.** TD-02 decomposed the
~1900-line `render()` into panel classes over the live `EditorState` model via a per-frame
`EditorContext`: `MainMenuBar`, `QuadListPanel`, `InspectorPanel` (context-sensitive Properties),
`VideoPlayerPanel`, and `ViewportEditor` (the non-`Panel`, pre-`NewFrame()` on-canvas warp
surface). `Application.cpp` 2411 → ~960 lines; `render()` body ~1910 → ~400.

## NEXT TASK — resume here (start v1.1 from `master`)
**Recommended: Color correction per quad** (US-104, P1, v1.1). Lowest-risk, best-fit first feature:
- A `Quad` property (brightness/contrast/gamma) + a few shader uniforms + an `InspectorPanel`
  section + an additive `.qmap` field (ADR-032 versioning already supports additive fields).
- No new dependencies; no success-metric risk.
- **Process:** open an ADR + write *checkable acceptance criteria* FIRST, then build in small
  build → test → verify (smoke) batches. Branch off `master`.

Alternatives: **soft-edge blending** (US-101, moderate rendering risk) or **MIDI/OSC control**
(US-102/103 — needs a NEW vcpkg dependency → an ADR + approval first, Change Boundary).

## Open debt (none blocking — see TECHNICAL_DEBT.md)
- **TD-09** — converge the two media models (MediaBin `SourceManager` vs quad-assignment
  `quadTextures`/`videoPlayer`) onto the source architecture. ⚠️ Touches the assignment path +
  on-canvas editing → success-metric risk; do as a **deliberate feature with its own ADR**.
- **TD-07** — build exit-code flake (gtest discovery timeout); touches CMake → needs approval.
- **TD-08** — defer audio snapshot reclamation to the control thread (LOW).

## The EditorContext contract (every panel uses it)
`EditorContext{ EditorState& es; Project*; MediaLibrary*; SourceManager*; OutputManager*; Renderer*; GLFWwindow* }`
— built once per frame in `render()`; panels take `render(EditorContext&)` and never store it;
panels are value members of `EditorState`; **ImGui window names preserved exactly** (imgui.ini
docking layout keys). `ViewportEditor` follows the contract but is **not** a `Panel` — it runs
pre-`NewFrame()` and draws via the OpenGL `Renderer`.

## Verification gate (non-negotiable)
`cmake --build build/debug` → `ctest --test-dir build/debug` (590/590; trust the "100% tests
passed" line — TD-07 build-exit-code flake) → `tests/smoke/drive_smoke.ps1` (warp + projector).
When media is touched, also `drive_media_smoke.ps1` (video) / `drive_image_smoke.ps1` (image) —
both maximize the window, clipboard-paste the media path, and capture editor + projector frames.

## Blockers
None. `master` is fully green, merged, and pushed.

## Reference links
- Decisions: @docs/adr/0034-render-decomposition.md · @docs/adr/0031-keep-vs-rebuild.md · @docs/adr/0032-qmap-migration.md · @docs/adr/0033-retire-multi-scene.md
- Roadmap: @QUAD_MAPPER_MVP_SPECIFICATION.md (§14.2 Post-MVP) · @docs/PRD.md (§3.2 / §4.2)
- Debt: @TECHNICAL_DEBT.md (TD-02 resolved; TD-07/08/09 open) · Success metric: @CLAUDE.md#success-metric
- Tasks: @TASKS.md · Machine state: @SESSION_STATE.json · Latest log: @docs/sessions/2026-06-16-td02-batch4-merge-media-verify.md

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
