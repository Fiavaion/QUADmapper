# Session 11 — TD-02: decompose render() into panels (5 of 6 batches)

**Date:** 2026-06-16
**Branch:** `refactor/app-decomposition` (PR #1)
**Focus:** TD-02 — break the ~1900-line `Application::render()` god-function into panel classes.

## How the session ran
1. **Scoping** (user: "scope out TD-02"). Mapped `render()` into separable blocks and found the
   blocker: the unwired `InspectorPanel`/`PreviewPanel` shells were written against a
   `SourceManager`/`TextureSource` **source model** the live `render()` never adopted (it uses a
   legacy `quadTextures` map + `videoPlayer`). "Wire up the shells" would mean finishing a
   half-done migration that touches the success-metric path.
2. **Decision** → [ADR-034](../adr/0034-render-decomposition.md): decompose over the **live
   model** (behavior-preserving); defer the source-arch convergence as **TD-09**. The
   vertex-editing block runs **before `ImGui::NewFrame()`** and draws via OpenGL, so it cannot be
   a docked `Panel` — it becomes a non-`Panel` `ViewportEditor` (batch 4).
3. **Model plan** (user: apply modelswitcher, "work optimally"). Opus for contract design +
   batch-3 review + batch 4; Sonnet via `ui-specialist` for mechanical extractions. Fable
   unavailable → Opus floor.
4. **Plan mode** → designed the `EditorContext` contract + the 4-batch (then 5-with-3b) sequence;
   approved.
5. **Execution**, batch-by-batch with a per-batch gate (build → ctest → smoke → commit).

## The contract (ADR-034)
- `struct EditorContext { EditorState& es; Project*; MediaLibrary*; SourceManager*;
  OutputManager*; Renderer*; GLFWwindow*; }` — built once per frame in `render()`, passed to each
  panel's `render(ctx)`; never stored by a panel.
- New panels are **value members of `EditorState`**, lazy-wired in render()'s init block.
- **ImGui window names preserved exactly** (imgui.ini docking layout keys).
- Function-`static` buffers → panel members. Cross-panel coordination travels through
  `ctx.es` (documented exception), staying behavior-identical.

## Batches completed (QM-054, 5 of 6)
| Batch | Extract | Commit |
|---|---|---|
| 0 | `EditorContext.hpp` + `Application::{newProject,saveProject,loadProject}` (de-dup 3× save/load) | (batch-0 + ADR commits) |
| 1 | `MainMenuBar` (File/Edit/Window) | `708d267` |
| 2 | `QuadListPanel` (list, z-order, groups, drag-drop) | `fb23972` |
| 3 | `InspectorPanel` = context-sensitive Properties (Quad/Output/Media), rebuilt on live model | `6d522af` |
| 3b | `VideoPlayerPanel` (kept as its own window — behavior-preserving) | `3a13950` |
| — | Checkpoint: restart package + tracking, pushed | `e5cb792` |

**Result:** `Application.cpp` 2411 → 1271 lines; `render()` body ~1910 → ~710. Every batch
build-clean + **590/590 ctest** + smoke-green (incl. F11 projector output,
`tests/smoke/out/05_projector_output.png`). Batches 1–3 got reviewer-mode diff checks (batch 3
was the Opus-review batch); all faithful moves.

## Remaining — BATCH 4 (the last; SUCCESS-METRIC CRITICAL)
Extract the pre-`ImGui::NewFrame()` viewport editing into a **non-`Panel` `ViewportEditor`**:
- Target: `render()` ~564–867 — quad render to main framebuffer + `CORNER PIN MODE` (~679) +
  `MESH EDIT MODE` (~771) + the keyboard-capture guard, all before `NewFrame()` (~878). Handles
  render via `m_renderer` (OpenGL), not ImGui draw lists.
- **Preserve verbatim:** frame ordering; `!io.WantTextInput && !io.WantCaptureKeyboard` guard;
  nudge amounts (`0.001` base, ×10 Shift, ×0.1 Alt; Tab/Shift+Tab cycle). **This is the success
  metric.**
- Delete `src/ui/panels/PreviewPanel.{hpp,cpp}` + its `src/ui/CMakeLists.txt` entry (dead shell);
  first confirm nothing still references it.
- Model: Opus + high in the **main session** (not Sonnet) + a fresh-context verifier subagent
  grading the diff against the success-metric criteria.
- Recommended in a **new context** — clean handoff via `notes/CURRENT_STATUS.md` + ADR-034 +
  `SESSION_STATE.json` `nextSteps[0]`.

## Verification gate (per batch)
`cmake --build build/debug` → `ctest --test-dir build/debug` (590/590; trust the "100% tests
passed" line — TD-07 build-exit-code flake) →
`powershell -ExecutionPolicy Bypass -File tests/smoke/drive_smoke.ps1 -ExePath C:\Users\jones\AIprojects\QUADmapper\build\debug\src\app\Debug\QUADmapper.exe`
(the absolute `-ExePath` avoids a PowerShell 5.1 `..`-traversal quirk).

## Pointers
- Contract/decision: [docs/adr/0034-render-decomposition.md](../adr/0034-render-decomposition.md)
- Restart package: [notes/CURRENT_STATUS.md](../../notes/CURRENT_STATUS.md)
- Debt: [TECHNICAL_DEBT.md](../../TECHNICAL_DEBT.md) (TD-02 in progress; TD-07/08/09 open)
- Approved plan (local, not in repo): `C:\Users\jones\.claude\plans\hidden-strolling-marshmallow.md`
