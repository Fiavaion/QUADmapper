---
name: keep-vs-rebuild-decision
description: "The 2026-06-14 decision to refactor QUADmapper in place rather than full-rebuild, and why"
metadata: 
  node_type: memory
  type: project
  originSessionId: c198be6b-ae9c-4e42-8b16-fc762b6cc1a8
---

On 2026-06-14 a full-rebuild-vs-keep question was raised for QUADmapper. A 5-specialist
read-only audit (renderer, video, audio, ui, core/architecture) **unanimously returned
REFACTOR-IN-PLACE at 85% confidence** → recorded in [docs/adr/0031-keep-vs-rebuild.md].

**Why:** The codebase rot is *localized to one function*, not architectural. Foundations
are solid (core/`Result`, scene data model, GL primitives, FFmpeg pimpl boundary, clean
build, 623/624 tests pass). The single root cause is `Application.cpp::render()` — a
~2034-line god-function with 25+ function-static "globals" that holds all scene state,
input, UI, save/load, and GL upload; `UIManager`/`SceneManager`/panels are instantiated
but bypassed (dual-track state). A rebuild would discard the working vertex-edit UX (the
success metric) and re-incur the same bugs for no structural gain.

**Progress (2026-06-15):** ✅ TD-02 step 1 (render() statics → owned `EditorState`) and
✅ TD-01 (projector output renders the warped scene; F11 sends to monitor 2) are DONE and
harness-verified — **the full success metric now passes end-to-end.** The verification gate
is `tests/smoke/drive_smoke.ps1` (drives the real app, screenshots monitors; read the PNGs
in tests/smoke/out/). Branch: `refactor/app-decomposition`.

**How to apply:** Do not propose a rebuild. Remaining sequence: (1) fix CRITICAL correctness
bugs — audio mutex/heap-alloc in RT callback (TD-03), video cross-thread data races (TD-04);
(2) TD-02 step 2 — wire `UIManager`/panels to dispatch, retire the dual-track
`m_sceneManager`/static-`Scene` split; (3) `.qmap` migration (TD-05); (4) delete dead code
(`FrameBuffer`, `DoubleBuffer`, `AudioEngine`/`UIManager` stubs — TD-06). Preserve the vertex
UX at `Application.cpp:674-808` and run the smoke harness after any UI/render change. See
ADR-031 / TECHNICAL_DEBT.md for file:line evidence.
