# Current Status

**Last updated:** 2026-06-16 (Session 12)
**Phase / focus:** ADR-031 refactor — **TD-02 `render()` decomposition COMPLETE** ([ADR-034](../docs/adr/0034-render-decomposition.md)) on branch `refactor/app-decomposition` (PR #1, ready to merge)
**Build:** ✅ app builds & launches   **Tests:** ✅ 590/590 (`ctest --test-dir build/debug`)   **Smoke:** ✅ green incl. F11 projector output

## What works right now (grounded, verified this session)
**TD-02 is done — all 6 decomposition batches landed.** Each was build-clean + 590/590 ctest + smoke-harness-green (the full success metric incl. `05_projector_output.png`), each its own commit on `refactor/app-decomposition`:
- **Batch 0** — `src/app/EditorContext.hpp` + `Application::{newProject,saveProject,loadProject}` (de-duped 3× inline save/load).
- **Batch 1** — `MainMenuBar` (`708d267`)
- **Batch 2** — `QuadListPanel` (`fb23972`)
- **Batch 3** — `InspectorPanel` rebuilt on the live model = context-sensitive Properties (`6d522af`)
- **Batch 3b** — `VideoPlayerPanel`, kept as its own dockable window (`3a13950`)
- **Batch 4** (last, success-metric-critical) — **`ViewportEditor`** (`f2dd41f`): the pre-`ImGui::NewFrame()` on-canvas warp surface (quad render + corner-pin/mesh-edit interaction + keyboard-capture guard) extracted **byte-for-byte** into a non-`Panel` class called from the same pre-NewFrame spot. Dead `PreviewPanel.{hpp,cpp}` shell + CMake entry deleted. A fresh-context Opus verifier confirmed the moved block is character-identical and all 7 success-metric criteria hold.

**`Application.cpp`: 2411 → ~960 lines** (−~1450). `render()` body: ~1910 → ~400 lines. The success metric (Tab-cycle + arrow-nudge vertex warp → F11 fullscreen projector) was **seen working** this session in `out/02_*.png` (warped quad, green selected handle) and `out/05_projector_output.png` (warped quad fullscreen on monitor 2).

## NEXT TASK — resume here
TD-02 is closed; PR #1 is green and ready. Pick one (none blocking — see [TECHNICAL_DEBT.md](../TECHNICAL_DEBT.md)):
1. **Merge PR #1** (`refactor/app-decomposition` → `master`).
2. **TD-09** — converge the two media models onto the source architecture. ⚠️ Touches the texture/video assignment path + on-canvas editing → success-metric regression risk; do as a **deliberate feature with its own ADR**, not debt cleanup.
3. **TD-07** — build exit-code flake (gtest discovery timeout). Touches CMake → Change Boundary, needs approval.
4. **TD-08** — defer audio snapshot reclamation to the control thread (LOW).

## The EditorContext contract (what every panel uses)
`EditorContext{ EditorState& es; Project*; MediaLibrary*; SourceManager*; OutputManager*; Renderer*; GLFWwindow* }` — built once per frame in `render()`; panels take `render(EditorContext&)` and never store it; panels are value members of `EditorState`; **ImGui window names preserved exactly** (imgui.ini docking layout keys). `ViewportEditor` follows the same contract but is **not** a `Panel` — it runs pre-`NewFrame()` and draws via the OpenGL `Renderer`, not ImGui.

## Verification gate (per batch, non-negotiable)
`cmake --build build/debug` → `ctest --test-dir build/debug` (590/590; trust the "100% tests passed" line — TD-07 build-exit-code flake) → `powershell -ExecutionPolicy Bypass -File tests/smoke/drive_smoke.ps1 -ExePath C:\Users\jones\AIprojects\QUADmapper\build\debug\src\app\Debug\QUADmapper.exe` (5 screenshots incl. `out/05_projector_output.png`).

## Blockers
None. Repo is fully green, committed, and pushed (`f2dd41f`).

## Reference links
- Decision: @docs/adr/0034-render-decomposition.md · @docs/adr/0031-keep-vs-rebuild.md · @docs/adr/0033-retire-multi-scene.md
- Debt: @TECHNICAL_DEBT.md (TD-02 resolved; TD-07, TD-08, TD-09 open) · Success metric: @CLAUDE.md#success-metric
- Tasks: @TASKS.md · Machine state: @SESSION_STATE.json

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
