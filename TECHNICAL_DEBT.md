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
| TD-09 | **Two media models (half-finished source migration).** `MediaBinPanel` uses `SourceManager`/`TextureSource`/`MediaItem`; quad texture/video *assignment* in `render()` uses the legacy `quadTextures` map + `EditorState::videoPlayer`. ADR-034 decomposes over the legacy model and defers convergence onto the source architecture here. Touches the texture/video assignment path + on-canvas editing → **success-metric regression risk; do as a deliberate feature, not debt cleanup.** **Now also blocks QM-055's on-quad camera display** (`CameraSource` lives in `SourceManager`, which the renderer doesn't read) — converging here lets camera + all sources display on quads in one stroke. | ~2 sessions | Architecture coherence + camera-on-quad (QM-055) | No |
| TD-07 | **Build exit-code flaky** — `gtest_discover_tests` 5s `TEST_DISCOVERY_TIMEOUT` race makes `cmake --build` return non-zero while tests are actually green (624/624 via `ctest --test-dir`). Touches CMake (Change Boundary → needs approval to fix). | 0.25 d | CI/`/test` reliability | No |
| TD-08 | **Audio snapshot reclamation** (residual of TD-03, LOW) — the lock-free source-list snapshot is a `shared_ptr`; when the source list changes mid-playback, the old snapshot's last ref can be released on the audio thread (one `free`, only at assign/remove-audio moments, never steady state). Proper fix: defer reclamation to the control thread (retire list). | 0.5 d | One possible glitch when audio is (re)assigned | No |

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
| TD-01 | 2026-06-15 | Projector output now renders the live warped scene. Per-output `Renderer` (own GL context) reuses `renderQuads`; `OutputManager::renderToOutputs(quadManager, textures)` called each frame from `Application::run()`; F11 auto-creates an output on the secondary monitor and enters perform mode. Placeholder gray-clear deleted. **Verified working:** `tests/smoke/drive_smoke.ps1` step 05 — F11 sends the warped quad fullscreen to monitor 2, captured clean on black (no UI/handles), matching the editor geometry. 624/624 tests pass. |
| TD-04 | 2026-06-16 | `currentFrame_`/`currentTime_`/`atEnd_` made `std::atomic` (single-writer ordering); fixes the cross-thread UB. Build clean, 624/624 tests. Commit `9830016`. |
| TD-03 | 2026-06-16 | RT callback path made lock-free + alloc-free: lock-free SPSC ring (`AudioBuffer`, atomic head/tail + acquire/release), atomic `shared_ptr` source-list + callback snapshots (`AudioMixer`, no mutex/alloc in `processAudio`), pre-allocated `readBuffer_` (`AudioSource`). Reviewed for memory ordering + null-snapshot (ctor publishes an empty list) + buffer sizing. Build clean; 156/156 audio tests + 624/624 full suite; app launches & runs the full smoke sequence without crashing. Residual reclamation nit tracked as TD-08. Follow-up fix: ring honored exact requested capacity (array still power-of-two) — earlier rounding broke the capacity contract (caught on full rebuild, LESSON-BUILD-007). |
| TD-05 | 2026-06-16 | `.qmap` versioning unified per [ADR-032](docs/adr/0032-qmap-migration.md). One `validateVersion()` policy now used by all four read paths (`load`/`deserialize`/`loadToSceneManager`/`deserializeScenes`): accept major ≤ 2, reject newer majors with an actionable message (previously the scene paths ignored `version` entirely — silent half-parse). v1.x flat files migrate into a single scene in-memory; v2.x files flatten into the legacy flat `Project` model instead of loading empty. The 4× copy-pasted quad-into-scene block collapsed to one `loadQuadInto()` helper that copies the **whole** geometry — fixing a latent bug where mesh-edit vertex offsets were silently dropped on scene load. No on-disk format change (writer still emits `2.0.0`). **Verified:** 631/631 tests (7 new: future/malformed-version reject, missing-version on scene path, v1→scene migration, v2→flat flatten, mesh-offset round-trip). |
| TD-06 | 2026-06-16 | Deleted dead code: `FrameBuffer`(video)/`DoubleBuffer`/`AudioEngine`/`UIManager`/renderer `Framebuffer`+`TextureCache` (files + CMakeLists entries + stale includes + Application `m_uiManager` wiring). Full build clean; 624/624; smoke harness passes end-to-end. |
| TD-02 | 2026-06-16 | `Application::render()` god-function decomposed per [ADR-034](docs/adr/0034-render-decomposition.md). Step 1 (statics → owned `EditorState`) + step 2 (ADR-033: one `Scene` of truth) + the UI decomposition into panel classes over the live `EditorState` model, via a per-frame `EditorContext` bundle. 6 build-clean batches, each 590/590 ctest + smoke-green (incl. F11 projector output): batch 0 `EditorContext` + `Application::{newProject,saveProject,loadProject}`; batch 1 `MainMenuBar` (`708d267`); batch 2 `QuadListPanel` (`fb23972`); batch 3 `InspectorPanel` = context-sensitive Properties (`6d522af`); batch 3b `VideoPlayerPanel` (`3a13950`); batch 4 (last, success-metric-critical) `ViewportEditor` — the pre-`NewFrame()` on-canvas warp surface extracted **byte-for-byte** (fresh-context Opus verifier confirmed), dead `PreviewPanel` shell deleted (`f2dd41f`). `Application.cpp` 2411 → ~960 lines; `render()` body ~1910 → ~400. **Verified working:** smoke harness 05 — Tab-cycle + arrow-nudge vertex warp sent fullscreen to the projector, captured warped on monitor 2. Remaining source-arch convergence is the separately-scoped TD-09 (not debt from this refactor). |
