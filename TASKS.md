# QUADmapper Tasks

Last Updated: 2026-06-16

## Quick Status
- In Progress: 0
- Todo: 3 debt (TD-07/08/09) + 6 AI (QM-056..QM-062 — Phase 9)
- Done: 56
- Blocked: 0

---

## Phase 9: AI Pre-Show Assist ("Snap-to-Surface")

ADR-035 **Accepted** (optional out-of-process Python sidecar; render path / build / `.qmap`
untouched). Plan: [docs/research/ai-preshow-roadmap-2026-06-16.md](docs/research/ai-preshow-roadmap-2026-06-16.md).
Grounded by the prior-art research, competitor audit, and a GO feasibility spike (all in
`docs/research/`). Each task: checkable AC first → small build→test→smoke batches. The wedge's
novelty is a hypothesis until **🚩 QM-061** (real-device test) passes.

#### QM-055: Live camera input as a media source (Phase 0 — enabling)
- **Status:** DONE (capture rail + PNG) — **Completed:** 2026-06-16. On-quad *display* deferred to TD-09 (user decision).
- **Phase:** Phase 9 · **Effort:** M · **Depends On:** none (capture rail for all AI features)
- **Delivered & verified on the real "HD User Facing" webcam (1280×720/30fps mjpeg):**
  - `VideoDecoder::openCamera()` — FFmpeg dshow capture; `libavdevice` linked (already in vcpkg, no new dep; CMake one-liner approved). `open()` refactored into shared `setupDecoderFromFormat()` (behavior-preserving; media smoke confirms video→quad→projector intact).
  - `CameraSource : TextureSource` (live, non-playable; drains to newest frame) + `CameraDevices` DirectShow enumeration + `SourceManager::createCameraSource`. Test group `test_source` (2 tests, real-hardware, skip-if-absent).
  - **AC2 (sidecar input):** `writeRgbPng()` — dependency-free PNG encoder (default vcpkg ffmpeg has no PNG encoder); **verified readable by OpenCV** (720×1280×3).
  - **AC3:** 592/592 ctest (+2); media smoke green.
- **Deferred (per user, → TD-09):** AC1's "camera displayed *on a quad*/projector" needs the renderer wired to `SourceManager` (the unconverged dual-model). Capture+PNG — the AI-roadmap-critical part — is complete; on-quad display rides on the deliberate TD-09 convergence.

#### QM-056: AI sidecar skeleton + IPC contract (ADR-035)
- **Status:** TODO · **Effort:** M · **Depends On:** QM-055
- **AC:** launch/attach/health-check/teardown `tools/ai-sidecar/`; versioned `POST /surface` stub round-trips; sidecar-absent disables AI UI cleanly with all existing workflows unaffected; contract test + Python unit test run GPU-free.

#### QM-057: Sidecar surface service (Metric3D → plane → 4 corners)
- **Status:** TODO · **Effort:** M · **Depends On:** QM-056
- **AC:** `POST /surface` returns dominant-surface corners + plane + confidence (Metric3D + Open3D segment_plane); unambiguous scene → encloses target, cluttered → low confidence (not confidently wrong); <3s GPU.

#### QM-058: "Propose corners" UI — human-in-the-loop confirm
- **Status:** TODO · **Effort:** M · **Depends On:** QM-057
- **AC:** capture → sidecar → draw proposed quad → user accepts (writes corner offsets) or nudges via Tab-cycle+arrows; low confidence prompts "tap the surface"; F11-projectable; smoke green; no `.qmap` change.

#### QM-059: Projector test-pattern emit + single-photo capture flow
- **Status:** TODO · **Effort:** S · **Depends On:** QM-055
- **AC:** flash a known rectangle/ChArUco fullscreen on the chosen output; single capture sees surface+pattern; round-trips to disk; smoke green.

#### QM-060: One-Flash homography solve → push warp
- **Status:** TODO · **Effort:** M · **Depends On:** QM-059, QM-057
- **AC:** `POST /oneflash` detects pattern+surface, returns homography as 4 corner offsets; applying it lands projected content aligned (no projector pose needed, one photo); `.qmap`-safe; smoke green.

#### QM-061: 🚩 Real-device validation gate (Phase 2 not "done" until this passes)
- **Status:** TODO · **Effort:** S · **Depends On:** QM-060
- **AC:** on a real projector + real surface + real phone photo, One-Flash produces a visibly correct warp, watched working; failure modes documented. Fallback if it fails: port MIT gray-code structured-light behind the same sidecar contract.

#### QM-062: Audience-perspective ("trap-art") anamorphic warp (Phase 3)
- **Status:** TODO · **Effort:** M · **Depends On:** QM-057
- **AC:** from one captured viewpoint, warp content to look correct from the phone's position; `.qmap`-safe; smoke green.

---

## In Progress

*No tasks currently in progress. Next: QM-056 (AI sidecar skeleton).*

## Done

#### QM-054: TD-02 — decompose render() into panels (ADR-034)
- **Status:** DONE (6 of 6 batches) — **Completed:** 2026-06-16
- **Notes:** ADR-034: decomposed the ~1900-line `render()` into panel classes over the live `EditorState` model (behavior-preserving), via a per-frame `EditorContext` bundle. All on `refactor/app-decomposition`, each build-clean + 590/590 ctest + smoke-green (incl. F11 projector output): batch 0 — `EditorContext.hpp` + `Application::{newProject,saveProject,loadProject}` (de-dup 3× save/load); batch 1 `MainMenuBar` (708d267); batch 2 `QuadListPanel` (fb23972); batch 3 `InspectorPanel` = context-sensitive Properties (6d522af); batch 3b `VideoPlayerPanel` (3a13950); **batch 4 (last, success-metric-critical) `ViewportEditor`** — the pre-`ImGui::NewFrame()` on-canvas warp surface (quad render + corner/mesh warp + keyboard-capture guard) extracted **byte-for-byte** into a non-`Panel` class; dead `PreviewPanel` shell deleted (f2dd41f). `Application.cpp` 2411→~960 lines; `render()` ~1910→~400. Verified: build clean, 590/590, smoke harness 05 (Tab-cycle + arrow-nudge warp sent fullscreen to the projector), fresh-context Opus verifier APPROVE. Source-arch convergence tracked separately as TD-09.

## Done

### Phase 8: ADR-031 Refactor (Session 10 — 2026-06-16)

#### QM-053: TD-02 step 2 — retire dual-track scene state (one scene of truth)
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** ADR-033. Deleted the unwired multi-scene machinery (`SceneManager` +35 tests, `ScenePanel`, scene-based `ProjectSerializer` methods, orphan `SceneManager` hooks in Inspector/Preview/Renderer, `m_sceneManager`). `EditorState::scene` is now the single source of truth. Reconciled render()'s 5 hand-rolled save/load sync sites into two shared helpers (`syncSceneToProject`/`syncProjectToScene`) using whole-geometry/whole-texture copies — fixing a latent bug that dropped mesh-edit vertex offsets + flipH/V on save/load. Build clean; 589/590 tests (1 pre-existing flaky timer test); success metric harness-verified (vertex warp + F11 projector output). Remaining TD-02 work: decompose render() UI into real panels.

#### QM-052: TD-05 — `.qmap` versioning, migration & save/load de-dup
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** ADR-032. One `validateVersion()` policy across all 4 read paths (accept major ≤ 2, reject newer majors loudly — scene paths previously ignored version). v1.x→single-scene migration; v2.x→flat-model flatten. Collapsed the 4× quad-copy into `loadQuadInto()` (whole-geometry copy), which also fixed a silent mesh-offset drop on scene load. No on-disk change. 631/631 tests (+7 new).

### Phase 8: ADR-031 Refactor (Session 9 — 2026-06-16)

#### QM-045: ADR-031 audit + decision + UI smoke harness
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** 5-specialist audit → refactor-in-place (not rebuild). Added `tests/smoke/drive_smoke.ps1` (drives the real app via Win32 scancode input, screenshots monitors) as the verification gate.

#### QM-046: TD-02 step 1 — extract render() statics → owned EditorState
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** ~40 function-statics → `src/app/EditorState.hpp`; body unchanged via same-named aliases. Behavior-preserved (harness + 624 tests).

#### QM-047: TD-01 — projector output renders the warped scene (SUCCESS METRIC)
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** Per-output `Renderer`; `renderToOutputs(quadManager, textures)` from `run()`; F11 auto-creates an output on the secondary monitor. Was a never-called gray-clear stub. Full success metric verified end-to-end.

#### QM-048: TD-04 — atomic cross-thread video decoder state
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** `currentFrame_`/`currentTime_`/`atEnd_` → `std::atomic` (fixes UB).

#### QM-049: TD-03 — lock-free + alloc-free audio RT callback
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** SPSC ring (`AudioBuffer`), atomic source/callback snapshots (`AudioMixer`), preallocated buffer (`AudioSource`). Residual reclamation nit → TD-08.

#### QM-050: TD-06 — delete dead code
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** Removed `FrameBuffer`/`DoubleBuffer`/`AudioEngine`/`UIManager`/renderer `Framebuffer`+`TextureCache` (files + CMake + wiring).

#### QM-051: Fix full-build regressions (AudioBuffer capacity, test signature)
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** Caught on full rebuild (LESSON-BUILD-007: full-build before ctest). AudioBuffer now honors exact capacity; OutputManagerTest updated to new signature.

### Phase 1: Core Foundation

#### QM-001: Setup CMake build system
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Medium
- **Depends On:** None
- **Completed:** 2025-12-19
- **Notes:**
  - vcpkg integration working (glfw3, glm, nlohmann-json)
  - Debug build successful (QUADmapper.exe)
  - Core tests passing (27/27)
  - External libs installed: GLAD, Dear ImGui (docking), stb_image

#### QM-002: Create GLFW window
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Small
- **Depends On:** QM-001
- **Completed:** 2025-12-19
- **Notes:**
  - 1920x1080 window with vsync
  - Framebuffer resize callback
  - Full main loop implementation

#### QM-003: Initialize OpenGL context
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Small
- **Depends On:** QM-002
- **Completed:** 2025-12-19
- **Notes:**
  - GLAD loader initialization working
  - OpenGL 3.3 core profile confirmed
  - Dear ImGui 1.92.6 with docking support
  - Multi-viewport enabled

#### QM-004: Implement single quad with 4-corner warping
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Large
- **Depends On:** QM-003
- **Completed:** 2025-12-19
- **Notes:**
  - QuadGeometry with bilinear interpolation
  - ShaderProgram class for OpenGL shaders
  - Renderer with mesh subdivision support (2-64 resolution)
  - Real-time corner warping

#### QM-005: Add image texture loading
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Medium
- **Depends On:** QM-003
- **Completed:** 2025-12-19
- **Notes:**
  - Texture class with stb_image integration
  - Support for PNG, JPG, TIF, BMP, TGA
  - GPU texture upload with mipmaps
  - UI for loading textures by path

#### QM-006: Create basic ImGui inspector panel
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Medium
- **Depends On:** QM-002
- **Completed:** 2025-12-19
- **Notes:**
  - "Quad Inspector" panel with corner controls
  - Mesh resolution slider (2-64)
  - Opacity control
  - Texture path input and load button

#### QM-007: Implement minimal project save/load
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Medium
- **Depends On:** QM-004, QM-005
- **Completed:** 2025-12-19
- **Notes:**
  - JSON format using nlohmann/json (.qmap files)
  - Saves quad corners, mesh resolution, opacity, texture paths
  - File menu with Save/Load and Ctrl+S/Ctrl+O shortcuts
  - Project panel with status display
  - Round-trip serialization verified with unit tests

### Phase 2: Mesh & Multi-Quad

#### QM-008: Implement mesh subdivision system
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 2
- **Estimated Effort:** Large
- **Depends On:** QM-004
- **Completed:** 2025-12-19
- **Notes:**
  - Configurable resolution (2-64)
  - Vertex buffer generation with proper vertex layout
  - Index buffer for triangle list rendering
  - MeshSubdivision utility class with static methods
  - Integration with QuadGeometry for mesh offsets

#### QM-009: Add mesh vertex editing mode
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 2
- **Estimated Effort:** Large
- **Depends On:** QM-008
- **Completed:** 2025-12-19
- **Notes:**
  - Vertex handle rendering with dedicated shaders
  - Drag interaction with mouse click/drag
  - Reset to bilinear option via UI button
  - MeshMode enum (CornersOnly vs MeshEdit)
  - Per-vertex offset storage in QuadGeometry

#### QM-010: Support multiple quads
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 2
- **Estimated Effort:** Medium
- **Depends On:** QM-004
- **Completed:** 2025-12-19
- **Notes:**
  - QuadManager class with full lifecycle management
  - Selection system with single/multi-select (Ctrl+click)
  - Select all (Ctrl+A), range select (Shift+click)
  - Selection callback support
  - Duplicate quad functionality

#### QM-011: Implement quad groups
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 2
- **Estimated Effort:** Medium
- **Depends On:** QM-010
- **Completed:** 2025-12-19
- **Notes:**
  - QuadGroup class with member ID list
  - Group visibility/lock propagation to members
  - Scene API for group management
  - Create group from selection
  - Drag-drop quads to groups in UI
  - 39 unit tests for group functionality

#### QM-012: Add Z-order management
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 2
- **Estimated Effort:** Small
- **Depends On:** QM-010
- **Completed:** 2025-12-19
- **Notes:**
  - Render order by z-order (lowest first)
  - QuadManager z-order operations (up/down/top/bottom)
  - Multi-select z-order operations
  - Keyboard shortcuts: Ctrl+]/[ (up/down), Ctrl+Shift+]/[ (top/bottom)
  - UI controls in Quad List panel

#### QM-013: Implement UV offset/scale controls
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 2
- **Estimated Effort:** Medium
- **Depends On:** QM-005
- **Completed:** 2025-12-19
- **Notes:**
  - UV offset (pan) with X/Y controls
  - UV scale (zoom) with X/Y controls
  - UV rotation support
  - Reset UV button in inspector
  - TextureBinding class for UV state
  - Shader support for UV transforms

### Phase 3: Video Playback

#### QM-014: Integrate FFmpeg video decoder
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 3
- **Estimated Effort:** Large
- **Depends On:** QM-001
- **Completed:** 2025-12-19
- **Notes:**
  - VideoDecoder class with pimpl pattern
  - FFmpeg libavcodec/libavformat integration
  - Background decoder thread with ring buffer
  - RGB24 frame output
  - Seek and frame navigation
  - FFmpeg 7.1.2 via vcpkg

#### QM-015: Implement video texture binding
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 3
- **Estimated Effort:** Medium
- **Depends On:** QM-014, QM-005
- **Completed:** 2025-12-19
- **Notes:**
  - FrameBuffer thread-safe ring buffer
  - Blocking/non-blocking push/pop operations
  - End of stream signaling
  - GPU texture upload with glTexSubImage2D
  - Texture wrapper for external GL textures

#### QM-016: Add playback controls
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 3
- **Estimated Effort:** Medium
- **Depends On:** QM-015
- **Completed:** 2025-12-19
- **Notes:**
  - VideoPlayer class with full controls
  - Play, pause, stop, togglePlayPause
  - Seek by time, progress, skip
  - Video Player UI panel with controls

#### QM-017: Implement speed control
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 3
- **Estimated Effort:** Small
- **Depends On:** QM-016
- **Completed:** 2025-12-19
- **Notes:**
  - Speed range 0.1x to 4.0x
  - Frame skipping for fast speeds
  - Speed slider in UI with 1x reset

#### QM-018: Add loop modes
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 3
- **Estimated Effort:** Small
- **Depends On:** QM-016
- **Completed:** 2025-12-19
- **Notes:**
  - LoopMode enum: Once, Loop, Ping-Pong
  - Loop mode combo box in UI
  - handleEndOfVideo with mode switching

#### QM-019: Implement timeline lock system
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 3
- **Estimated Effort:** Medium
- **Depends On:** QM-018
- **Completed:** 2025-12-19
- **Notes:**
  - TimelineSyncManager class
  - Lock/unlock players to timeline
  - Wait for all locked videos to finish
  - Synchronized restart

---

## In Progress

*No tasks currently in progress*

---

## Todo

### Phase 4: Multi-Output

#### QM-020: Create output window system
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 4
- **Estimated Effort:** Large
- **Depends On:** QM-003
- **Completed:** 2025-12-19
- **Notes:**
  - OutputWindow class with GLFW window management
  - Shared OpenGL context via create(GLFWwindow* shareContext)
  - Fullscreen/windowed toggle with saved position
  - OutputManager with monitor enumeration (MonitorInfo struct)
  - Perform mode enter/exit
  - 50 unit tests (27 OutputWindowTest + 22 OutputManagerTest + MonitorInfo)

#### QM-021: Add resolution/rotation controls
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 4
- **Estimated Effort:** Medium
- **Depends On:** QM-020
- **Completed:** 2025-12-19
- **Notes:**
  - ResolutionPreset struct with common presets (720p, 1080p, 4K, etc.)
  - getEffectiveResolution() accounts for rotation (swaps w/h for 90/270)
  - getRotationDegrees()/setRotationDegrees() conversion methods
  - applyResolution() applies to GLFW window

#### QM-022: Implement monitor assignment
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 4
- **Estimated Effort:** Medium
- **Depends On:** QM-020
- **Completed:** 2025-12-19
- **Notes:**
  - MonitorInfo::getDisplayString() for UI-friendly display
  - getMonitor(index), getPrimaryMonitor() methods
  - MonitorChangeCallback with setMonitorChangeCallback()
  - enableMonitorChangeDetection() with GLFW callback

#### QM-023: Add Perform Mode
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 4
- **Estimated Effort:** Medium
- **Depends On:** QM-022
- **Completed:** 2025-12-19
- **Notes:**
  - goBorderlessFullscreen() for borderless mode
  - KeyCallback with setKeyCallback() on OutputWindow
  - handleF11Key(), handleEscapeKey(), togglePerformMode()
  - setupPerformModeKeyCallbacks() for automatic handling

---

### Phase 5: Scene System

#### QM-024: Implement scene data structure
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 5
- **Estimated Effort:** Medium
- **Depends On:** QM-010
- **Completed:** 2025-12-19
- **Notes:**
  - SceneViewState struct (panOffset, zoomLevel, gridVisible, snapToGrid)
  - Scene::getViewState(), resetView() methods
  - SceneManager: getSceneByIndex, duplicateScene, generateUniqueSceneName, reset
  - ProjectSerializer: saveWithScenes/loadToSceneManager with v2.0 format
  - Backward compatibility for v1.x project files
  - 21 new SceneManager unit tests

#### QM-025: Add scene creation/deletion
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 5
- **Estimated Effort:** Small
- **Depends On:** QM-024
- **Completed:** 2025-12-19
- **Notes:**
  - canDeleteScene() safety check (can't delete last scene)
  - getActiveSceneId(), getSceneIndex() methods
  - setActiveSceneByIndex() for keyboard shortcuts 1-9
  - moveScene() for drag-to-reorder

#### QM-026: Implement scene switching
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 5
- **Estimated Effort:** Medium
- **Depends On:** QM-025
- **Completed:** 2025-12-19
- **Notes:**
  - Instant switch via setActiveScene(UUID)
  - setActiveSceneByIndex() for keyboard 1-9
  - Click-to-select in ScenePanel UI

#### QM-027: Create scene panel UI
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 5
- **Estimated Effort:** Medium
- **Depends On:** QM-026
- **Completed:** 2025-12-19
- **Notes:**
  - ScenePanel.hpp/cpp with full ImGui implementation
  - New/Duplicate/Delete buttons with disabled states
  - Active scene indicator (green ">")
  - Inline rename (double-click), context menu
  - Drag-to-reorder with ImGui drag-drop

---

### Phase 6: Audio System

#### QM-028: Extract audio from video
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 6
- **Estimated Effort:** Medium
- **Depends On:** QM-014
- **Completed:** 2025-12-19
- **Notes:**
  - AudioFrame struct (interleaved float samples, channels, sampleRate, pts)
  - AudioInfo struct for stream metadata
  - AudioBuffer thread-safe ring buffer with blocking/non-blocking ops
  - AudioDecoder with pimpl pattern for FFmpeg isolation
  - SwrContext for converting to float format
  - Background decoding thread support
  - 54 audio tests (17 AudioFrame + 21 AudioBuffer + 16 AudioDecoder)

#### QM-029: Implement audio mixer
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 6
- **Estimated Effort:** Large
- **Depends On:** QM-028
- **Completed:** 2025-12-19
- **Notes:**
  - AudioMixer.hpp/cpp with miniaudio backend
  - Thread-safe mixing of multiple AudioSources
  - Master volume (linear and dB), master mute
  - Source management (add/remove/get/clear)
  - Pimpl pattern isolates miniaudio headers

#### QM-030: Add per-source volume controls
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 6
- **Estimated Effort:** Small
- **Depends On:** QM-029
- **Completed:** 2025-12-19
- **Notes:**
  - AudioSource.hpp/cpp with volume (0.0-1.0) and dB conversion
  - Mute toggle per source
  - Pan control (-1.0 to 1.0) with constant power panning
  - Left/right gain calculation in readSamples()

#### QM-031: Implement multi-output routing (basic)
- **Status:** DONE
- **Priority:** P1
- **Phase:** Phase 6
- **Estimated Effort:** Medium
- **Depends On:** QM-029
- **Completed:** 2025-12-19
- **Notes:**
  - AudioDevice.hpp/cpp with miniaudio device enumeration
  - AudioDeviceInfo struct (id, name, isDefault, channels, sampleRates)
  - Device selection by ID or default
  - Sample rate, channel count, buffer size configuration
  - Pimpl pattern for miniaudio isolation

---

### Phase 7: Polish

#### QM-032: Implement keyboard shortcut system
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-002
- **Completed:** 2025-12-20
- **Notes:**
  - KeyboardShortcuts.hpp/cpp with ShortcutAction enum (51 actions)
  - ModifierFlags (Ctrl, Shift, Alt, Super) with KeyCombination
  - Default Resolume-style bindings, conflict detection
  - JSON save/load to user config directory
  - 52 unit tests

#### QM-033: Create preferences dialog
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-006
- **Completed:** 2025-12-20
- **Notes:**
  - PreferencesDialog.hpp/cpp with 3 tabs (General, Shortcuts, Performance)
  - Interactive shortcut capture with 5s timeout
  - AppPreferences struct with JSON save/load
  - Settings saved to %APPDATA%\QUADmapper\ or ~/.config/quadmapper/

#### QM-034: Add undo/redo system
- **Status:** DONE
- **Priority:** P1
- **Phase:** Phase 7
- **Estimated Effort:** Large
- **Depends On:** QM-010
- **Completed:** 2025-12-20
- **Notes:**
  - CommandHistory.hpp/cpp with execute/undo/redo
  - Command merging for drag operations
  - Compound commands for grouped operations
  - Concrete commands: MoveCorner, MoveMeshVertex, CreateQuad, DeleteQuad, etc.
  - 40 unit tests (17 CommandHistory + 23 Commands)

#### QM-035: Implement media bin panel
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-005, QM-015
- **Completed:** 2025-12-20
- **Notes:**
  - MediaBinPanel.hpp/cpp with thumbnail grid
  - Import button with Windows native file dialog
  - Drag-drop source for quad assignment (MEDIA_ITEM_ID payload)
  - Context menu (Remove, Reveal in Explorer, Copy Path)
  - 24 MediaLibrary tests

#### QM-036: Add preview panel with interaction
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Large
- **Depends On:** QM-004, QM-009
- **Completed:** 2025-12-20
- **Notes:**
  - PreviewPanel.hpp/cpp with real-time scene preview
  - Edit modes: Select, Corners, Mesh
  - Corner/mesh vertex drag editing with handles
  - Zoom (mouse wheel), pan (middle-drag)
  - Grid overlay, selection highlighting

#### QM-037: Create output panel
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-020
- **Completed:** 2025-12-20
- **Notes:**
  - OutputPanel.hpp/cpp with output list
  - Perform Mode button (Enter/Exit) with color state
  - Enable/disable toggles, Add/Remove buttons
  - Monitor dropdown, resolution presets, rotation
  - Inline rename, context menu

#### QM-038: Performance optimization pass
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Large
- **Depends On:** All previous
- **Completed:** 2025-12-20
- **Notes:**
  - PerformanceMonitor.hpp/cpp with frame timing
  - Section timing (render, update, UI phases)
  - Draw call/triangle counting, texture memory tracking
  - FPS history (min/max/avg)
  - ScopedTimer, PERF_SCOPE macros
  - 12 unit tests

#### QM-039: Error handling and logging
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** All previous
- **Completed:** 2025-12-20
- **Notes:**
  - ErrorNotification.hpp/cpp with ErrorNotificationManager
  - Severity levels (Debug, Info, Warning, Error, Critical)
  - Auto-expiring toast notifications (5s default, configurable)
  - Enhanced Logger with severity filtering and file output
  - 18 unit tests

#### QM-040: Create unit tests for core
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-001
- **Completed:** 2025-12-20
- **Notes:**
  - UUIDTest.cpp: 8 tests (generate, copy, assignment, toString, fromString, hashing)
  - ResultTest.cpp: 8 tests (success/error, valueOr, map, andThen)
  - LoggerTest.cpp: 4 tests (level ordering, format strings)
  - TimerTest.cpp: 5 tests (start/stop, elapsed, reset)
  - CommandHistoryTest.cpp: 17 tests (execute, undo/redo, merge, compound)
  - PerformanceMonitorTest.cpp: 12 tests (frame timing, FPS, section timing)
  - ErrorNotificationTest.cpp: 18 tests (notifications, severity, auto-expire)
  - Total: 72 core unit tests

#### QM-041: Create unit tests for scene
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-010
- **Completed:** 2025-12-20
- **Notes:**
  - QuadTest.cpp: 12 tests (creation, properties, visibility, lock)
  - QuadGeometryTest.cpp: 18 tests (corners, bilinear interpolation, mesh offsets)
  - MeshSubdivisionTest.cpp: 15 tests (vertex generation, index generation, resolution)
  - SceneTest.cpp: 22 tests (add/remove quads, groups, visibility)
  - SceneManagerTest.cpp: 35 tests (scene lifecycle, switching, serialization)
  - QuadManagerTest.cpp: 28 tests (selection, z-order, multi-select)
  - QuadGroupTest.cpp: 39 tests (members, visibility propagation, lock)
  - CommandsTest.cpp: 23 tests (move corner, create/delete quad, undo/redo)
  - Total: 192 scene unit tests

#### QM-042: Create unit tests for project
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-007
- **Completed:** 2025-12-20
- **Notes:**
  - ProjectTest.cpp: 18 tests (project state, dirty flag, path handling)
  - ProjectSerializerTest.cpp: 32 tests (save/load, v2.0 format, v1.x backward compat)
  - MediaLibraryTest.cpp: 24 tests (add/remove media, thumbnails, metadata)
  - Round-trip serialization verified with multiple scenarios
  - Total: 74 project unit tests

#### QM-043: Create integration tests
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Large
- **Depends On:** QM-040, QM-041, QM-042
- **Completed:** 2025-12-20
- **Notes:**
  - RenderingPipelineTest.cpp: Tests shader compilation, texture binding, mesh rendering
  - ProjectIntegrationTest.cpp: Full project save/load with all components
  - OutputWindowTest.cpp: Window creation, context sharing, fullscreen
  - OutputManagerTest.cpp: Multi-output management, perform mode
  - AudioMixerTest.cpp: Multi-source mixing, volume control
  - AudioDeviceTest.cpp: Device enumeration, output routing
  - KeyboardShortcutsTest.cpp: Shortcut binding, conflict detection
  - Total: 286 integration/component tests

#### QM-044: Manual test protocols
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Small
- **Depends On:** All previous
- **Completed:** 2025-12-20
- **Notes:**
  - Created docs/MANUAL_TEST_PROTOCOLS.md
  - Visual verification checklist (18 items across 5 categories)
  - Performance benchmark tests (startup, frame rate, memory)
  - Multi-output test procedure (window, perform mode, monitor detection)
  - Scene system tests (management, switching)
  - Audio system tests (playback, device selection)
  - Project save/load tests
  - Input system tests (keyboard, mouse)
  - Error handling tests

---

## Blocked

*No blocked tasks*

---

## Done

*No completed tasks*

---

## Task ID Reference

| ID | Title | Status | Phase |
|----|-------|--------|-------|
| QM-001 | Setup CMake build system | DONE | Phase 1 |
| QM-002 | Create GLFW window | DONE | Phase 1 |
| QM-003 | Initialize OpenGL context | DONE | Phase 1 |
| QM-004 | Implement single quad with 4-corner warping | DONE | Phase 1 |
| QM-005 | Add image texture loading | DONE | Phase 1 |
| QM-006 | Create basic ImGui inspector panel | DONE | Phase 1 |
| QM-007 | Implement minimal project save/load | DONE | Phase 1 |
| QM-008 | Implement mesh subdivision system | DONE | Phase 2 |
| QM-009 | Add mesh vertex editing mode | DONE | Phase 2 |
| QM-010 | Support multiple quads | DONE | Phase 2 |
| QM-011 | Implement quad groups | DONE | Phase 2 |
| QM-012 | Add Z-order management | DONE | Phase 2 |
| QM-013 | Implement UV offset/scale controls | DONE | Phase 2 |
| QM-014 | Integrate FFmpeg video decoder | DONE | Phase 3 |
| QM-015 | Implement video texture binding | DONE | Phase 3 |
| QM-016 | Add playback controls | DONE | Phase 3 |
| QM-017 | Implement speed control | DONE | Phase 3 |
| QM-018 | Add loop modes | DONE | Phase 3 |
| QM-019 | Implement timeline lock system | DONE | Phase 3 |
| QM-020 | Create output window system | DONE | Phase 4 |
| QM-021 | Add resolution/rotation controls | DONE | Phase 4 |
| QM-022 | Implement monitor assignment | DONE | Phase 4 |
| QM-023 | Add Perform Mode | DONE | Phase 4 |
| QM-024 | Implement scene data structure | DONE | Phase 5 |
| QM-025 | Add scene creation/deletion | DONE | Phase 5 |
| QM-026 | Implement scene switching | DONE | Phase 5 |
| QM-027 | Create scene panel UI | DONE | Phase 5 |
| QM-028 | Extract audio from video | DONE | Phase 6 |
| QM-029 | Implement audio mixer | DONE | Phase 6 |
| QM-030 | Add per-source volume controls | DONE | Phase 6 |
| QM-031 | Implement multi-output routing | DONE | Phase 6 |
| QM-032 | Implement keyboard shortcut system | DONE | Phase 7 |
| QM-033 | Create preferences dialog | DONE | Phase 7 |
| QM-034 | Add undo/redo system | DONE | Phase 7 |
| QM-035 | Implement media bin panel | DONE | Phase 7 |
| QM-036 | Add preview panel with interaction | DONE | Phase 7 |
| QM-037 | Create output panel | DONE | Phase 7 |
| QM-038 | Performance optimization pass | DONE | Phase 7 |
| QM-039 | Error handling and logging | DONE | Phase 7 |
| QM-040 | Create unit tests for core | DONE | Phase 7 |
| QM-041 | Create unit tests for scene | DONE | Phase 7 |
| QM-042 | Create unit tests for project | DONE | Phase 7 |
| QM-043 | Create integration tests | DONE | Phase 7 |
| QM-044 | Manual test protocols | DONE | Phase 7 |
| QM-052 | TD-05 .qmap versioning, migration & save/load de-dup | DONE | Phase 8 |
| QM-053 | TD-02 step 2 retire dual-track scene state (one scene of truth) | DONE | Phase 8 |
| QM-054 | TD-02 decompose render() into panels (ADR-034, 6 batches) | DONE | Phase 8 |
| QM-055 | AI pre-show: live camera input + PNG capture (on-quad display → TD-09) | DONE | Phase 9 |
| QM-056 | AI sidecar skeleton + IPC contract (ADR-035) | TODO | Phase 9 |
| QM-057 | Sidecar surface service (Metric3D → plane → corners) | TODO | Phase 9 |
| QM-058 | "Propose corners" UI (human-in-the-loop confirm) | TODO | Phase 9 |
| QM-059 | Projector test-pattern emit + single-photo capture | TODO | Phase 9 |
| QM-060 | One-Flash homography solve → push warp | TODO | Phase 9 |
| QM-061 | 🚩 Real-device validation gate | TODO | Phase 9 |
| QM-062 | Audience-perspective anamorphic warp | TODO | Phase 9 |

---
