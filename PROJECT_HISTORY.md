# QUADmapper Project History

This document maintains a chronological record of all development activities, decisions, and changes made to the QUADmapper project.

---

## Session Log

### Session 12 - 2026-06-16 - TD-02 batch 4 (last): ViewportEditor — render() decomposition COMPLETE

**Focus:** The final, success-metric-critical batch of TD-02 (QM-054). Plan-first, then
execute directly in the main Opus session per the resume brief.

**Completed (QM-054 → DONE):**
- **Batch 4** (`f2dd41f`): extracted the pre-`ImGui::NewFrame()` viewport-editing block
  (quad render to the main framebuffer + corner-pin/mesh-edit interaction + the
  keyboard-capture guard, render() ~567–873) into a new **non-`Panel`** class
  `quadmapper::ViewportEditor` (`src/ui/panels/ViewportEditor.{hpp,cpp}`), called via
  `es.viewportEditor.render(ctx)` from the same pre-NewFrame spot. The moved body is
  **byte-for-byte identical**, wrapped in same-named aliases (`m_renderer`/`m_window`/
  `m_project`/`quadManager`/corner-mesh-snap-video fields) — LESSON-ARCH-006.
- Removed the now-unused corner/mesh/snap + `videoAssignedQuadId` aliases from `render()`
  (kept the video-pump aliases still in use).
- Deleted the dead `PreviewPanel.{hpp,cpp}` source-arch shell + its CMake entry; added
  `ViewportEditor` to `quadmapper_ui`.

**Deviation from ADR-0034's table** (which named `PreviewPanel` as the home): per the
session-11 resume brief, a non-`Panel` `ViewportEditor` + delete-the-shell — consistent with
the batch 3b `VideoPlayerPanel` deviation. Noted in the commit.

**Verification (the gate, all green):** build clean (only pre-existing `strncpy` C4996
warnings, none in the new file) → `ctest` 590/590 (4 audio integration skipped) →
`drive_smoke.ps1` 5 screenshots incl. `05_projector_output.png` (visibly warped quad
fullscreen on monitor 2; `02_*.png` shows the keyboard-nudged corner with the green selected
handle) → **fresh-context Opus verifier: APPROVE**, all 7 success-metric criteria, byte-for-byte
block comparison (306 lines, zero diff).

**Net:** `Application.cpp` 2411 → ~960 lines; `render()` body ~1910 → ~400. **TD-02 closed**
(moved to Resolved in TECHNICAL_DEBT.md). PR #1 (`refactor/app-decomposition`) green and
ready to merge. Remaining open debt (none blocking): TD-07, TD-08, TD-09.

---

### Session 11 - 2026-06-16 - TD-02: decompose render() into panels (5 of 6 batches)

**Focus:** The last open ADR-031 item — TD-02, decomposing the ~1900-line `render()`
god-function into real panel classes. User asked to scope it, then to apply the model-switcher
rubric and "work optimally." Result: an ADR + contract + a sequenced, model-tiered batch plan,
executed batch-by-batch with per-batch verification.

**Decisions:**
- [ADR-034](docs/adr/0034-render-decomposition.md): decompose over the **live `EditorState`
  model** (behavior-preserving) rather than finishing the half-done `SourceManager` migration the
  unwired `InspectorPanel`/`PreviewPanel` shells assume. The migration is logged separately as
  **TD-09**. Key finding: the vertex-editing block runs **before `ImGui::NewFrame()`** and renders
  via OpenGL, so it cannot become a docked `Panel` — it gets a non-`Panel` `ViewportEditor`.
- Contract: a per-frame `EditorContext` bundle (`EditorState&` + manager pointers) passed to each
  panel's `render(ctx)`; panels are value members of `EditorState`; ImGui window names preserved
  (imgui.ini layout keys).
- Model schedule (modelswitcher): Opus for the contract design + batch-3 review + batch 4;
  Sonnet (via `ui-specialist`) for the mechanical extractions. Fable confirmed unavailable → Opus floor.
- Batch 3b deviation: kept the Video Player as its own `VideoPlayerPanel` window instead of
  "folding" it into the Inspector (the plan's wording) — folding would orphan the
  `showVideoPlayer` toggle / window layout key; behavior-preservation wins.

**Completed (QM-054, in progress — 5 of 6 batches):**
- Batch 0: `src/app/EditorContext.hpp` + extracted New/Save/Load into
  `Application::{newProject,saveProject,loadProject}` (de-duped the 3× inline save/load).
- Batch 1: `MainMenuBar` (`708d267`).
- Batch 2: `QuadListPanel` (`fb23972`).
- Batch 3: `InspectorPanel` rebuilt on the live model = context-sensitive Properties
  (Quad/Output/Media) (`6d522af`) — Opus diff-review confirmed faithful.
- Batch 3b: `VideoPlayerPanel` (`3a13950`).
- Checkpoint: restart package + tracking updated, pushed (`e5cb792`).

**Verified working:** every batch was build-clean + **590/590 ctest** + smoke-harness green
(the full success metric incl. `tests/smoke/out/05_projector_output.png`). `Application.cpp`
2411→1271 lines; `render()` body ~1910→~710.

**Shipped:** commits `708d267`, `fb23972`, `6d522af`, `3a13950`, `e5cb792` (+ batch-0/ADR
commits) pushed to `refactor/app-decomposition` (PR #1).

**Handoff / next steps:** **Batch 4 (last; success-metric critical)** — extract the
pre-`NewFrame()` viewport editing (quad render + corner/mesh warp + keyboard-capture guard,
`render()` ~564–867) into a non-`Panel` `ViewportEditor`; delete the dead `PreviewPanel` shell.
Recommended in a **fresh context**, **Opus + high in the main session** (not Sonnet) + a
fresh-context verifier subagent vs the success metric. Full brief in
[notes/CURRENT_STATUS.md](notes/CURRENT_STATUS.md); contract in
[docs/adr/0034-render-decomposition.md](docs/adr/0034-render-decomposition.md). Then TD-07
(CMake, needs approval), TD-08 (LOW), TD-09 (source-arch convergence).

---

### Session 10 - 2026-06-16 - Debt cleanup: .qmap versioning + retire multi-scene track

**Focus:** Two ADR-031 debt items — `.qmap` versioning/migration (TD-05) and "one scene of
truth" (TD-02 step 2). User delegated task selection ("what do you recommend as a master
computer engineer"); both picked on data-integrity / dead-code grounds.

**Completed:**
- QM-052: TD-05 — unified `.qmap` version policy across all 4 read paths (accept major ≤ 2,
  reject newer with an actionable message; the scene paths previously ignored version
  entirely). v1.x→single-scene and v2.x→flat migration; de-duped the copy-pasted save/load
  into shared helpers; fixed a silent mesh-offset drop on scene load. [ADR-032](docs/adr/0032-qmap-migration.md).
- QM-053: TD-02 step 2 — retired the unwired multi-scene track. Deleted `SceneManager`
  (+35 tests), `ScenePanel`, the scene-based `ProjectSerializer` methods, the orphan
  `SceneManager` hooks in Inspector/Preview/Renderer, and `Application::m_sceneManager`.
  `EditorState::scene` is now the single source of truth. Reconciled render()'s 5 hand-rolled
  `Scene`↔`Project` save/load sites into two shared whole-geometry/texture helpers
  (`syncSceneToProject`/`syncProjectToScene`) — fixing a latent mesh-offset + flipH/V drop on
  save/load. Net −1648 lines. [ADR-033](docs/adr/0033-retire-multi-scene.md).
- Fixed the pre-existing flaky `PerformanceMonitorTest.MinMaxFrameTime` (asserted a sub-
  granularity threshold; broke under Windows' default ~15.6ms timer). Now asserts min/max
  tracking instead.

**Key decisions:**
- [ADR-032](docs/adr/0032-qmap-migration.md): additive, backward-compatible `.qmap` versioning;
  no on-disk format change (writer stays at the format the app already emits).
- [ADR-033](docs/adr/0033-retire-multi-scene.md): delete the non-functional multi-scene
  subsystem rather than wire it up; git + the preserved v2 read path allow restoring it later
  as a deliberate feature.

**Verified working:** full build clean; **590/590 tests** (4 audio integration skipped — need a
real video file); success metric harness-verified end-to-end (vertex warped to 0.500/0.400;
F11 → warped quad fullscreen-clean on the projector, `tests/smoke/out/05_projector_output.png`).

**Shipped:** 3 commits (`cc6febd`, `669a05c`, `172acec`) pushed to `refactor/app-decomposition`
(PR #1).

**Handoff / next steps:** TD-02 remaining — decompose the ~2000-line `render()` into real panels
(`InspectorPanel`/`PreviewPanel` are now unwired shells). Then TD-07 (build exit-code flake —
CMake, needs approval) and TD-08 (audio snapshot reclamation, LOW). See
[notes/CURRENT_STATUS.md](notes/CURRENT_STATUS.md) and [TECHNICAL_DEBT.md](TECHNICAL_DEBT.md).

---

### Session 9 - 2026-06-16 - ADR-031 Refactor: success metric working + critical fixes

**Focus:** Audit-driven keep-and-refactor (ADR-031). Got the full success metric working
end-to-end and resolved all CRITICAL/HIGH audit items.

**Completed:**
- QM-045: ADR-031 5-specialist audit → refactor-in-place; UI smoke harness (`tests/smoke/`)
- QM-046: TD-02 step 1 — render() statics → owned `EditorState`
- QM-047: TD-01 — projector output renders the warped scene (success metric complete)
- QM-048: TD-04 — atomic cross-thread video decoder state (UB fix)
- QM-049: TD-03 — lock-free + alloc-free audio RT callback
- QM-050: TD-06 — deleted dead code
- QM-051: fixed two full-build regressions (AudioBuffer capacity contract; stale test signature)

**Key decisions:**
- [ADR-031](docs/adr/0031-keep-vs-rebuild.md): refactor-in-place, not rebuild (rot localized to
  `Application::render()`; foundations sound). Accepted.
- Verification gate = `tests/smoke/drive_smoke.ps1` (drives the real app; reads screenshots).

**Verified working:** add quad → Tab-cycle vertex → arrow-nudge (Shift 10×) → F11 → warped
quad fullscreen on the projector. 624/624 tests on a full build.

**Lessons:** LESSON-BUILD-007 (full-build before ctest — partial builds run stale test exes,
hid two regressions); LESSON-TEST-004 (GLFW maps keys by scancode, not VK); LESSON-ARCH-006
(de-static a god-function via owned struct + same-named aliases).

**Shipped:** branch `refactor/app-decomposition` pushed; PR #1 opened against master.

**Handoff / next steps:** TD-05 (.qmap migration + de-dup save/load), TD-02 step 2 (real
UIManager/panel dispatch + retire dual-track `m_sceneManager`/static-`Scene`), TD-07 (build
exit-code flake — CMake, needs approval), TD-08 (audio snapshot reclamation, LOW). See
[notes/CURRENT_STATUS.md](notes/CURRENT_STATUS.md) and [TECHNICAL_DEBT.md](TECHNICAL_DEBT.md).

---

### Session 5 - 2025-12-19 - Phase 3 Complete

**Focus:** FFmpeg build verification and video player UI integration

**Completed:**
- QM-014: FFmpeg video decoder - BUILD VERIFIED
- QM-015: Video texture binding - BUILD VERIFIED
- QM-016: Playback controls - UI INTEGRATED
- QM-017: Speed control - UI INTEGRATED
- QM-018: Loop modes - UI INTEGRATED
- QM-019: Timeline lock system - BUILD VERIFIED

**Technical Achievements:**
- FFmpeg 7.1.2 successfully installed via vcpkg
- All video components compile without errors
- Fixed LOG macro warnings (variadic macro support)
- Integrated VideoPlayer into Application.cpp
- Added Video Player panel with full controls:
  - Play/Pause/Stop buttons
  - Skip forward/backward (5s)
  - Progress slider with seek
  - Speed control (0.1x - 4.0x)
  - Loop mode selector (Once/Loop/Ping-Pong)
  - Video info display
  - Assign video to quad functionality
- Added Texture::setGLTextureId for external texture support
- 203/203 tests still passing

**Decisions Made:**
- **ADR-013:** Texture class supports external GL texture IDs (non-owning mode)

**Files Created/Modified:**
- `src/core/Logger.hpp` - Variadic LOG macros
- `src/renderer/Texture.hpp/.cpp` - External texture ID support
- `src/app/Application.cpp` - Video player integration and UI panel
- `src/video/VideoDecoder.cpp` - Removed unused variable warning

**Handoff Notes:**
- **Phase 3 COMPLETE** - All 6/6 tasks done
- Application builds and runs with video support
- Ready for Phase 4, 5, or 6 (can run in parallel)
- Next suggested: QM-020 (Output window system) or QM-024 (Scene data structure)

---

### Session 4 - 2025-12-19 - Phase 2 Completion + Phase 3 Code Complete

**Focus:** Multi-quad support, mesh editing, groups, z-order, UV controls, then video playback

**Part 1: Phase 2 Completed**
- QM-008: Mesh subdivision system (formalized)
- QM-009: Mesh vertex editing mode
- QM-010: Multiple quads with selection system
- QM-011: Quad groups
- QM-012: Z-order management
- QM-013: UV offset/scale controls

**Part 2: Phase 3 Code Complete (Build Pending)**
- QM-014: FFmpeg video decoder - CODE COMPLETE
- QM-015: Video texture binding (FrameBuffer) - CODE COMPLETE
- QM-016: Playback controls - CODE COMPLETE
- QM-017: Speed control - CODE COMPLETE
- QM-018: Loop modes - CODE COMPLETE
- QM-019: Timeline lock system - CODE COMPLETE

**Technical Achievements:**
- QuadManager class with full lifecycle and multi-selection
- QuadGroup with member ID list and visibility/lock propagation
- Mesh vertex editing with draggable handles and bilinear reset
- Z-order operations (up/down/top/bottom) with keyboard shortcuts
- UV transform support (offset, scale, rotation)
- 39 new QuadGroup tests, 203/203 total tests passing
- 6 parallel sub-agents completed all Phase 2 tasks
- VideoDecoder with FFmpeg integration (pimpl pattern)
- VideoPlayer with full play/pause/stop/seek/speed/loop controls
- FrameBuffer thread-safe ring buffer for frame staging
- TimelineSyncManager for synchronized video restart
- FFmpeg added to vcpkg.json (building in background)

**Decisions Made:**
- **ADR-008:** QuadManager handles multi-selection and z-order operations
- **ADR-009:** QuadGroup uses member ID list (not ownership)
- **ADR-010:** Mesh vertex editing via draggable handles with bilinear reset
- **ADR-011:** VideoDecoder uses pimpl pattern for FFmpeg isolation
- **ADR-012:** Background decoding thread with ring buffer

**Files Created/Modified:**
- `src/scene/QuadManager.hpp/.cpp` - Quad collection management
- `src/scene/QuadGroup.hpp/.cpp` - Group with member management
- `src/scene/Scene.hpp/.cpp` - Group management API
- `src/renderer/Renderer.hpp/.cpp` - Mesh handle rendering
- `src/shaders/handle.vert/.frag` - Handle circle shaders
- `src/project/ProjectSerializer.cpp` - Group serialization
- `src/app/Application.cpp` - Major UI updates
- `tests/unit/scene/QuadGroupTest.cpp` - 39 group tests
- `src/video/VideoDecoder.hpp/.cpp` - FFmpeg video decoding
- `src/video/VideoPlayer.hpp/.cpp` - High-level player controls
- `src/video/FrameBuffer.hpp/.cpp` - Thread-safe frame ring buffer
- `src/video/VideoPlaybackState.hpp/.cpp` - Playback state struct
- `src/video/TimelineSyncManager.hpp/.cpp` - Synchronized playback
- `src/video/CMakeLists.txt` - FFmpeg detection and linking
- `vcpkg.json` - Added FFmpeg dependency

**Handoff Notes:**
- **Phase 2 COMPLETE** - All 6/6 tasks done
- **Phase 3 CODE COMPLETE** - All 6/6 tasks implemented
- FFmpeg vcpkg build in progress (15-30 min first time, then cached)
- Next session: run `cmake --preset debug` to complete configuration
- Then build and verify video components compile
- Finally integrate VideoPlayer into Application.cpp

---

### Session 3 - 2025-12-19 - Phase 1 Completion

**Focus:** Project save/load implementation, completing Phase 1

**Completed:**
- QM-007: Implement minimal project save/load (.qmap JSON format)

**Technical Achievements:**
- ProjectSerializer with full quad geometry serialization
- JSON format with version compatibility checking
- File menu with New/Save/Load and Ctrl+N/S/O shortcuts
- Project panel with file path input and status display
- Fixed and updated test suite (95/95 tests passing)

**Decisions Made:**
- **ADR-007:** Project stores quads directly (Scene integration deferred to Phase 2)

**Files Created/Modified:**
- `src/project/Project.hpp` - Added quad and texture path storage
- `src/project/ProjectSerializer.cpp` - Full quad serialization implementation
- `src/app/Application.cpp` - Added File menu and Project panel
- `tests/unit/project/ProjectTest.cpp` - Complete rewrite with serialization tests
- `tests/unit/scene/SceneTest.cpp` - Updated to match Scene API
- `tests/unit/scene/QuadGeometryTest.cpp` - Updated to match QuadGeometry API
- `tests/unit/scene/MeshSubdivisionTest.cpp` - Updated to match MeshSubdivision API

**Handoff Notes:**
- **Phase 1 COMPLETE** - All 7/7 tasks done
- Application can save/load projects with quad corners, mesh settings, and texture paths
- 95/95 unit tests passing
- Ready to begin Phase 2: Mesh & Multi-Quad

---

### Session 2 - 2025-12-19 - Phase 1 Core Implementation

**Focus:** GLFW window, OpenGL rendering, quad warping, texture loading, ImGui inspector

**Completed:**
- QM-001: Setup CMake build system (external dependencies downloaded)
- QM-002: Create GLFW window (1920x1080 with vsync)
- QM-003: Initialize OpenGL context (3.3 Core via GLAD)
- QM-004: Implement single quad with 4-corner warping
- QM-005: Add image texture loading (stb_image integration)
- QM-006: Create basic ImGui inspector panel

**Technical Achievements:**
- ShaderProgram class for compiling/linking GLSL shaders
- Texture class with stb_image for PNG/JPG/BMP/TGA loading
- Renderer with mesh subdivision (2-64 resolution)
- Real-time corner warping with bilinear interpolation
- ImGui docking and multi-viewport enabled
- Core tests: 27/27 passing

**Decisions Made:**
- **ADR-004:** Shader sources embedded via CMake configure_file()
- **ADR-005:** Mesh resolution range 2-64 for quad subdivision
- **ADR-006:** Default quad covers 10%-90% of screen space

**Files Created/Modified:**
- `src/app/Application.cpp` - Full GLFW/OpenGL/ImGui initialization and main loop
- `src/renderer/Renderer.cpp` - Quad rendering with mesh subdivision
- `src/renderer/ShaderProgram.cpp` - GLSL shader compilation
- `src/renderer/Texture.cpp` - Image loading with stb_image
- `src/shaders/CMakeLists.txt` - Shader embedding via file(READ)
- `external/imgui/*` - Downloaded Dear ImGui docking branch (12 files)
- `external/glad/*` - Installed GLAD OpenGL 3.3 Core loader
- `external/stb/stb_image.h` - Downloaded stb_image header

**Handoff Notes:**
- Phase 1 is 6/7 complete (QM-007 project save/load remaining)
- Application renders a warpable quad with texture support
- Quad Inspector panel provides corner, mesh, and opacity controls
- Ready to implement project serialization for QM-007

---

### Session 1 - 2025-12-19 - Initial Project Setup

**Focus:** Development infrastructure setup

**Completed:**
- Created complete project directory structure
- Set up CMake build system with vcpkg integration
- Created all component CMakeLists.txt files
- Wrote CLAUDE.md development guide
- Created formal PRD and EDD documents
- Set up session management system
- Configured sub-agent automation infrastructure

**Decisions Made:**
- **ADR-001:** Using hybrid sub-agent strategy (phase-first, component-parallel)
- **ADR-002:** Session management via /start and /end slash commands
- **ADR-003:** Task tracking with QM-XXX ID format in TASKS.md

**Files Created:**
- `CMakeLists.txt` - Root build configuration
- `vcpkg.json` - Dependency manifest
- `CMakePresets.json` - Build presets
- `CLAUDE.md` - Development guide
- `docs/PRD.md` - Product Requirements Document
- `docs/EDD.md` - Engineering Design Document
- `PROJECT_HISTORY.md` - This file
- `TASKS.md` - Task tracking
- `SESSION_STATE.json` - Auto-continue state
- `.claude/commands/*.md` - Slash commands
- All component `CMakeLists.txt` files

**Handoff Notes:**
- Project structure is complete and ready for implementation
- Next step: Begin Phase 1 (Core Foundation) starting with QM-001
- Build system not yet tested (requires vcpkg dependencies)

---

## Major Milestones

| Date | Milestone | Notes |
|------|-----------|-------|
| 2025-11-23 | Specification Complete | QUAD_MAPPER_MVP_SPECIFICATION.md finalized |
| 2025-12-19 | Infrastructure Setup | PRD, EDD, build system, session management |
| 2025-12-19 | Phase 1 Complete | All 7 tasks done: rendering, textures, inspector, save/load |
| 2025-12-19 | Phase 2 Complete | All 6 tasks done: multi-quad, groups, z-order, mesh editing, UV controls |
| 2025-12-19 | Phase 3 Complete | All 6 tasks done: FFmpeg decoder, video player, UI integration |

---

## Architecture Decisions Record (ADR)

### ADR-001: Sub-Agent Strategy
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Need to efficiently parallelize development work across phases
**Decision:** Use hybrid phase-first, component-parallel strategy with automatic spawn triggers
**Consequences:** Enables parallel development of independent components while respecting dependencies

### ADR-002: Session Management
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Need to maintain development continuity across sessions with context limits
**Decision:** Use /start and /end slash commands with three tracking files (PROJECT_HISTORY.md, TASKS.md, SESSION_STATE.json)
**Consequences:** Enables automatic context recovery and seamless session continuation

### ADR-003: Task ID Format
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Need consistent task identification across tracking systems
**Decision:** Use QM-XXX format (e.g., QM-001, QM-042)
**Consequences:** Clear, sequential, project-specific task identification

### ADR-008: QuadManager Selection System
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Need to manage multiple quads with selection and z-order operations
**Decision:** QuadManager handles multi-selection, z-order changes, and lifecycle management
**Consequences:** Clean separation of quad collection management from rendering

### ADR-009: QuadGroup Membership Model
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Need to organize quads into groups without transferring ownership
**Decision:** QuadGroup stores member IDs (not ownership), quads can be in multiple groups
**Consequences:** Flexible grouping without complex ownership hierarchies

### ADR-010: Mesh Vertex Editing
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Need to allow fine-grained mesh deformation beyond corner warping
**Decision:** Draggable vertex handles with per-vertex offsets, bilinear reset option
**Consequences:** Enables detailed surface correction while preserving original geometry

### ADR-011: VideoDecoder Pimpl Pattern
**Date:** 2025-12-19
**Status:** Accepted
**Context:** FFmpeg headers are complex and pollute namespace
**Decision:** Use pimpl pattern to isolate FFmpeg types in implementation file
**Consequences:** Clean public interface, faster compilation, easier to swap backends

### ADR-012: Background Video Decoding
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Video decoding is CPU-intensive and would block main thread
**Decision:** Background thread with ring buffer for frame staging
**Consequences:** Smooth playback, decoupled decode/render timing

### ADR-013: External Texture ID Support
**Date:** 2025-12-19
**Status:** Accepted
**Context:** Video frames use OpenGL textures managed by Application, need to pass to Texture wrapper
**Decision:** Texture class supports setGLTextureId for non-owning external textures
**Consequences:** Video textures can be used with existing quad rendering without duplicating texture management

---
