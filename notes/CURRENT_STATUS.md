# Current Status

**Last updated:** 2026-06-14
**Phase / focus:** Refactor-in-place per [ADR-031](../docs/adr/0031-keep-vs-rebuild.md) — branch `refactor/app-decomposition`
**Build:** ✅ app builds (`QUADmapper.exe`) & launches clean (GL 3.3, RTX 3080, 2 monitors)   **Tests:** 624/624 pass (`ctest --test-dir build/debug`)

## What works right now (grounded, verified this session)
- [x] App builds, launches, main loop runs, default quad created — no crash
- [x] 624/624 unit/integration tests pass (4 audio integration skipped — need a real video file)
- [x] **Vertex-warp UX VERIFIED WORKING** via `tests/smoke/drive_smoke.ps1`: Tab/Shift+Tab cycle + arrow nudge + Shift 10× all move corners by the exact expected normalized amounts (TL→0.500,0.400; TR→0.550,0.300; BR→0.700,0.680). The app source was untouched — it was correct.
- [x] **UI smoke harness built** (`tests/smoke/drive_smoke.ps1`) — drives the real app via Win32 scancode input, screenshots each monitor; the autonomous verification gate for the refactor.

## What's broken / unverified
- ❌ **Projector output does NOT work** — `OutputManager::renderToOutputs()` is a gray-clear stub and is never called; `m_outputManager->update()` is commented out. The fullscreen-to-projector half of the success metric is unimplemented. (TD-01)
- ⚠️ **`render()` god-function** (~2034 lines, ~40 statics) — the foundation blocker (TD-02).
- ⚠️ Build exit-code flaky from gtest discovery timeout (TD-07) — tests are green, the build *return code* lies.

## Next 3 tasks (in order — ADR-031 sequencing)
1. **TD-02 (in progress)** — ✅ Step 1 done: ~40 render() statics extracted into owned `EditorState` member ([src/app/EditorState.hpp](../src/app/EditorState.hpp)); behavior-preserved (harness verified, 624/624 tests). Remaining: wire `UIManager`+panels to dispatch, retire the dual-track `m_sceneManager`/static-`Scene` split so there's one scene of truth.
2. **TD-01** Wire the real render path: `Renderer::renderScene()` → FBO → `OutputManager::renderToOutputs()`; **re-verify the projector gate** (extend the harness to drive output + capture monitor 2).
3. **TD-03/TD-04** Correctness: audio RT-safety + video data races.

## Acceptance criteria for the refactor (the gate)
- The full 2-minute success-metric workflow runs end-to-end on a real run: add quad → Tab/Shift+Tab cycle vertex → arrow nudge (Shift 10× / Alt 0.1×) → **send fullscreen to a chosen projector output and SEE it warped correctly.**
- 624/624 tests still green; no new `TODO`/`DEFERRED`/`TEMPORARY` in critical paths; no dual-track state left.

## Blockers
- None. (The interactive-verification blocker is resolved: `tests/smoke/drive_smoke.ps1` is the gate. It covers the vertex workflow now; extend it to assert projector output once TD-01 wires it — that needs an output-creation path the harness can drive, e.g. a CLI/keyboard "send to output N".)

## Reference links
- Decision: @docs/adr/0031-keep-vs-rebuild.md   ·   Debt: @TECHNICAL_DEBT.md   ·   Success metric: @CLAUDE.md#success-metric
- Tasks: @notes/TODOs.md   ·   Lessons: @notes/lessons.md   ·   Machine state: @SESSION_STATE.json

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
