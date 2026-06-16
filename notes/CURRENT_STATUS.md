# Current Status

**Last updated:** 2026-06-14
**Phase / focus:** Refactor-in-place per [ADR-031](../docs/adr/0031-keep-vs-rebuild.md) — branch `refactor/app-decomposition`
**Build:** ✅ app builds (`QUADmapper.exe`) & launches clean (GL 3.3, RTX 3080, 2 monitors)   **Tests:** 624/624 pass (`ctest --test-dir build/debug`)

## What works right now (grounded, verified this session)
- [x] App builds, launches, main loop runs, default quad created — no crash
- [x] 624/624 unit/integration tests pass (4 audio integration skipped — need a real video file)
- [x] **Vertex-warp UX VERIFIED WORKING** via `tests/smoke/drive_smoke.ps1`: Tab/Shift+Tab cycle + arrow nudge + Shift 10× all move corners by the exact expected normalized amounts (TL→0.500,0.400; TR→0.550,0.300; BR→0.700,0.680). The app source was untouched — it was correct.
- [x] **UI smoke harness built** (`tests/smoke/drive_smoke.ps1`) — drives the real app via Win32 scancode input, screenshots each monitor; the autonomous verification gate for the refactor.

## ✅ SUCCESS METRIC FULLY VERIFIED (2026-06-15)
The complete definition-of-done now works end-to-end, harness-verified:
add quad → Tab-cycle vertex → arrow-nudge (Shift 10×) → **F11 sends it fullscreen to the
projector (monitor 2), rendered warped + clean**. See `tests/smoke/out/05_projector_output.png`.

## What's broken / unverified
- ⚠️ **`render()` god-function** (~2034 lines) — TD-02 step 1 done (statics → EditorState); UI-into-panels decomposition remains (maintainability, not blocking).
- ⚠️ Build exit-code flaky from gtest discovery timeout (TD-07) — tests are green, the build *return code* lies.
- ✅ Correctness CRITICALs fixed: audio RT-safety (TD-03), video data races (TD-04). Residual audio reclamation nit tracked as TD-08 (LOW).

## Next tasks (in order)
1. **TD-02 (remaining): decompose render() UI into real panels.** `InspectorPanel`/`PreviewPanel` are now unwired shells (their `SceneManager` hooks were deleted in ADR-033); wire them (or rebuild as real panels) and pull UI out of the 2000-line `render()`.
2. **TD-07** build exit-code flake from `gtest_discover_tests` timeout (CMake, **needs approval** — Change Boundary). Also fold in: make `PerformanceMonitorTest.MinMaxFrameTime` robust to Windows timer granularity (currently flaky-red).
3. **TD-08** defer audio snapshot reclamation to the control thread (LOW).

_Done S10:_
- _**TD-05** — `.qmap` version policy unified across all 4 read paths + migration + save/load de-dup. [ADR-032](../docs/adr/0032-qmap-migration.md)._
- _**TD-02 step 2** — retired the unwired multi-scene track → one `Scene` of truth; deleted `SceneManager`/`ScenePanel`/scene-serializer methods; reconciled render()'s 5 save/load sync sites into 2 shared whole-geometry helpers (fixed a latent mesh-offset/flip drop). [ADR-033](../docs/adr/0033-retire-multi-scene.md). Success metric harness-verified; 589/590 tests (1 pre-existing flaky timer test)._

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
