# ADR-031: Keep & refactor-in-place vs. full rebuild

**Date:** 2026-06-14
**Status:** Accepted (ratified by user 2026-06-14)
**Deciders:** user (animaphic), Claude (5-specialist audit synthesis)

## Context

A decision was raised: full rebuild of QUADmapper vs. continue on the existing v1.0.0
codebase (~25k LOC src, ~7k LOC tests, 7 phases "complete", 623/624 tests passing). A
read-only health sweep was run with five specialist personas (renderer, video, audio, ui,
core/architecture). The choice is hard to reverse: a rebuild discards the working
vertex-edit UX (the success metric) and the solid foundation layer; keeping commits us to
refactoring a known god-object.

## Decision

**Refactor-in-place. Do NOT rebuild.** All five specialists returned KEEP /
REFACTOR-IN-PLACE at 85% confidence. The rot is *localized*, not architectural.

## Findings (evidence)

**Solid, keep as-is:**
- `core/` (UUID, Logger, `Result<T,Error>`, Types) — clean, well-tested.
- Scene data model (`Quad`, `QuadGeometry`, `MeshSubdivision`) — well-factored, correct.
- GL primitives (`ShaderProgram`, `Texture`, `OutputWindow`) — correct RAII, GL 3.3 floor honored, shared-context multi-window correct.
- FFmpeg pimpl boundary — clean, no libav* leakage downstream.
- Audio architecture *shape* — pimpl, atomics on hot params, SPSC ring intent.
- Build is clean; CMake/vcpkg sane; 623/624 tests pass (1 flaky timing test).

**The single root cause (shows up in every domain's report):**
- `Application.cpp::render()` is a ~2034-line god-function with 25+ function-static
  "globals" holding ALL scene state, input, UI, save/load, and GL upload
  (`Application.cpp:398-2432`). `UIManager`, `SceneManager`, and the panel classes are
  instantiated but **bypassed** — dual-track state that will silently diverge.

**Correctness bugs to fix (not rebuild — fix):**
- AUDIO CRITICAL: mutex acquire + `std::vector` copy + heap alloc inside the RT callback (`AudioMixer.cpp:197-244`, `AudioSource.cpp:113`, `AudioBuffer.cpp:94`) → guaranteed glitches under load.
- VIDEO CRITICAL: unsynchronized cross-thread `atEnd_`/`currentTime_`/`currentFrame_` (`VideoDecoder.cpp:245,383-385`) → UB.
- RENDER/OUTPUT HIGH: `Renderer::renderScene()` and `OutputManager::renderToOutputs()` are TODO stubs (`Renderer.cpp:324`, `OutputManager.cpp:372`) — multi-output projection to a second screen is **not actually wired**; current output path is ad-hoc inline. ⚠️ This means the "fullscreen to chosen projector output" half of the success metric needs re-verification by running the app.
- PROJECT HIGH: no `.qmap` version migration (one-way reject at `ProjectSerializer.cpp:396-398`); save/load logic copy-pasted 3×.
- Dead code: `FrameBuffer`/`DoubleBuffer` (video), `AudioEngine` stub, `UIManager` stub, `Framebuffer`/`TextureCache` empty headers.

## Consequences

- ✅ Keeps the verified vertex-edit UX and the solid foundation; no reset to zero.
- ✅ Problems are concentrated and have a clear, sequenced fix path.
- ❌ Commits to decomposing `Application::render()` — a real, planned refactor (not a
  surgical diff), with the vertex-edit UX (`Application.cpp:674-808`) preserved verbatim.
- ❌ Several CRITICAL correctness bugs must be fixed before the relevant features can be
  called production-solid.

## Recommended sequencing (gates the rest)

1. **Decompose `Application::render()`** — promote statics to members, kill the dual-track
   `SceneManager`/static-`Scene` split, wire `UIManager` + panels. Preserve vertex UX exactly.
2. **Wire the real render path** — implement `Renderer::renderScene()` → FBO →
   `OutputManager::renderToOutputs()`; re-verify the fullscreen-to-output success-metric gate.
3. **Correctness fixes** — audio RT-safety (lock-free SPSC, pre-alloc buffers), video data races (atomics).
4. **Hygiene** — delete dead code; `.qmap` migration dispatch; de-dupe save/load.

## Effort estimate (refactor-in-place to production-solid)

Renderer/output ~6–8 d · UI ~5 d · Audio ~1–2 wk · Video ~2–3 d · Application.cpp
decomposition ~3–4 sessions. Overlapping critical path (decomposition is shared) →
**~3–5 weeks focused**. A rebuild would be months and discard verified behavior.

## Alternatives considered

1. **Full rebuild** — rejected: discards solid foundations + working vertex UX, re-incurs
   the same correctness bugs from scratch, resets all verified behavior to zero. No
   structural gain — the problem is one function, not the architecture.
2. **Hybrid (keep foundation, rebuild app/UI layer)** — rejected as overkill: the
   decomposition in step 1 achieves the same clean app/UI layer without throwing away the
   working `render()` logic and panels that already exist.
