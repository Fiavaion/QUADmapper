# Current Status

**Last updated:** 2026-06-16
**Phase / focus:** ADR-031 refactor — **TD-02 `render()` decomposition** ([ADR-034](../docs/adr/0034-render-decomposition.md)) on branch `refactor/app-decomposition` (PR #1)
**Build:** ✅ app builds & launches   **Tests:** ✅ 590/590 (`ctest --test-dir build/debug`)   **Smoke:** ✅ green incl. F11 projector output

## What works right now (grounded, verified this session)
**5 of 6 TD-02 decomposition batches done.** Each was build-clean + 590/590 ctest + smoke-harness-green (the full success metric incl. `05_projector_output.png`), each its own commit on `refactor/app-decomposition`:
- **Batch 0** — `src/app/EditorContext.hpp` (the per-frame state/managers bundle panels receive) + extracted New/Save/Load into `Application::{newProject,saveProject,loadProject}` (de-duped the 3× inline save/load).
- **Batch 1** — `MainMenuBar` (`708d267`)
- **Batch 2** — `QuadListPanel` (`fb23972`)
- **Batch 3** — `InspectorPanel` rebuilt on the live model = context-sensitive Properties (Quad/Output/Media) (`6d522af`) — got the Opus diff-review; faithful move.
- **Batch 3b** — `VideoPlayerPanel`, kept as its own dockable window (`3a13950`). *Deviation from the plan's "fold into Inspector": chosen for behavior-preservation — folding would have orphaned the `showVideoPlayer` toggle / "Video Player" layout key.*

**`Application.cpp`: 2411 → 1271 lines** (−1140). `render()` body: ~1910 → ~710 lines.

## NEXT TASK — resume here: BATCH 4 (the last one; SUCCESS-METRIC CRITICAL)
Extract the **pre-`ImGui::NewFrame()` viewport editing** into a `ViewportEditor` class and delete the dead `PreviewPanel` shell.
- **Target:** `render()` ~lines **564–867** — quad render to the main framebuffer + `CORNER PIN MODE` (~679) + `MESH EDIT MODE` (~771) + the keyboard-capture guard, all **before** `ImGui::NewFrame()` (~878). Handles render via `m_renderer` (OpenGL), **not** ImGui draw lists.
- `ViewportEditor` is **NOT a `Panel`** (panels run post-NewFrame). Call it from the same pre-NewFrame spot. **Preserve verbatim:** frame ordering; the `!io.WantTextInput && !io.WantCaptureKeyboard` guard; nudge amounts (`0.001` base, ×10 Shift, ×0.1 Alt; Tab/Shift+Tab cycle). **This is the success metric — do not regress it.**
- **Delete** `src/ui/panels/PreviewPanel.{hpp,cpp}` + its `src/ui/CMakeLists.txt` entry (dead source-arch shell, ADR-0034).
- **Model:** Opus + high, done **directly in the main session (not delegated to Sonnet)**, plus a **fresh-context verifier subagent** grading the diff against the success-metric criteria.
- **Full contract + block map + batch table:** [docs/adr/0034-render-decomposition.md](../docs/adr/0034-render-decomposition.md). Approved plan file (local, not in repo): `C:\Users\jones\.claude\plans\hidden-strolling-marshmallow.md`.

## The EditorContext contract (inherited by every batch)
`EditorContext{ EditorState& es; Project*; MediaLibrary*; SourceManager*; OutputManager*; Renderer*; GLFWwindow* }` — built once per frame in `render()`; panels take `render(EditorContext&)` and never store it; new panels are value members of `EditorState`; **ImGui window names preserved exactly** (imgui.ini docking layout keys).

## Verification gate (per batch, non-negotiable)
`cmake --build build/debug` → `ctest --test-dir build/debug` (590/590; trust the "100% tests passed" line — TD-07 build-exit-code flake) → `powershell -ExecutionPolicy Bypass -File tests/smoke/drive_smoke.ps1 -ExePath C:\Users\jones\AIprojects\QUADmapper\build\debug\src\app\Debug\QUADmapper.exe` (5 screenshots incl. `out/05_projector_output.png`).

## Blockers
None. Repo is fully green and committed.

## Reference links
- Decision: @docs/adr/0034-render-decomposition.md · @docs/adr/0031-keep-vs-rebuild.md · @docs/adr/0033-retire-multi-scene.md
- Debt: @TECHNICAL_DEBT.md (TD-02 in progress; TD-07, TD-08, TD-09 open) · Success metric: @CLAUDE.md#success-metric
- Tasks: @TASKS.md · Machine state: @SESSION_STATE.json

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
