# Technical Debt Tracker

> Zero-tolerance project: this file should normally be near-empty. It exists to make any
> accepted deferral **visible and costed**, not to make debt routine. Core/foundation work
> is never deferred — it compounds 4–7× to fix later. `/start` should halt new feature work
> if any critical item below is open.

## Active debt

Classified by the 2026-06-14 five-specialist audit ([ADR-031](docs/adr/0031-keep-vs-rebuild.md)).
All are **refactor-in-place** items; severity-ordered, fixed per the ADR sequencing.

| ID | Item | Est. cost to fix | Impact | Critical path? |
|----|------|------------------|--------|----------------|
| TD-01 | **Projector output not wired** — `OutputManager::renderToOutputs()` is a gray-clear stub AND is never called; `m_outputManager->update()` is commented out ([Application.cpp:391](src/app/Application.cpp#L391)). The fullscreen-to-projector HALF of the success metric does not work. | 1 d (after TD-02) | **Breaks definition-of-done** | **Yes** |
| TD-02 | **`Application::render()` god-function** — ~2034 lines, ~40 function-static "globals" hold all scene/input/UI/save-load/GL state; `UIManager`/`SceneManager`/panels instantiated but bypassed (dual-track state). Gates TD-01 and all else. | ~3–4 sessions | Blocks all feature work | **Yes (foundation)** |
| TD-03 | **Audio RT-safety** — mutex + `std::vector` copy + heap alloc inside the audio callback ([AudioMixer.cpp:197-244](src/audio/AudioMixer.cpp#L197-L244), [AudioSource.cpp:113](src/audio/AudioSource.cpp#L113)). Glitches under load. | 1–2 wk | Audio unusable under load | No |
| TD-04 | **Video data races** — unsynchronized cross-thread `atEnd_`/`currentTime_`/`currentFrame_` ([VideoDecoder.cpp:245,383](src/video/VideoDecoder.cpp#L245)). UB. | 1–2 d | Correctness (UB) | No |
| TD-05 | **No `.qmap` migration** — one-way version reject ([ProjectSerializer.cpp:396-398](src/project/ProjectSerializer.cpp#L396-L398)); save/load copy-pasted 3×. | 0.5 d | First format change breaks all saved projects | No |
| TD-06 | **Dead code** — `FrameBuffer`/`DoubleBuffer` (video, orphaned), `AudioEngine` stub, `UIManager` stub, empty `Framebuffer.hpp`/`TextureCache.hpp`. | 0.5 d | Maintenance hazard / divergence | No |
| TD-07 | **Build exit-code flaky** — `gtest_discover_tests` 5s `TEST_DISCOVERY_TIMEOUT` race makes `cmake --build` return non-zero while tests are actually green (624/624 via `ctest --test-dir`). Touches CMake (Change Boundary → needs approval to fix). | 0.25 d | CI/`/test` reliability | No |

Remaining minor TODO: [PreferencesDialog.cpp:174](src/ui/dialogs/PreferencesDialog.cpp#L174) — "Open folder browser dialog" (minor UX, not critical path).

## Deferral decision (fill in BEFORE accepting any deferral)

- **Item:** <what>
- **Is it core / user-facing / a foundation others build on?** If yes → **do not defer.**
- **Features that will be built on top:** <0–1 low risk · 2–5 document · 5+ high risk>
- **Cost now vs later:** <Nh now> vs <~Nh × (1 + 0.5 × phases_on_top) later>
- **Justification & mitigation:** <why, and how the risk is contained>

## Resolved debt (evidence required — never "tests pass" alone)

| ID | Resolved | Evidence (fix + verified-working check + no TODO left) |
|----|----------|--------------------------------------------------------|
| | | |
