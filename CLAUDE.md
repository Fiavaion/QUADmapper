# QUADmapper Development Guide

This document provides development instructions for Claude Code agents working on QUADmapper.

## Project Overview

QUADmapper is a professional C++17 Windows projection mapping application built with OpenGL, Dear ImGui, and FFmpeg. It enables users to warp video and image content onto physical surfaces using perspective-corrected quads with mesh subdivision.

**Target Users:** Live event technicians, installation artists, VJs, architectural projection designers, theater/stage designers.

## Build System

- **Build Tool:** CMake 3.20+
- **Compiler:** MSVC 17 (Visual Studio 2022)
- **Package Manager:** vcpkg (manifest mode)
- **Standard:** C++17

### Quick Commands

```powershell
# Configure (first time)
cmake --preset debug

# Build
cmake --build build/debug

# Run tests
ctest --preset debug

# Run application
./build/debug/QUADmapper.exe
```

### Prerequisites

1. Visual Studio 2022 with C++ workload
2. vcpkg installed with VCPKG_ROOT environment variable set
3. CMake 3.20+

## Directory Structure

| Directory | Purpose | Owner |
|-----------|---------|-------|
| `src/core/` | Core utilities: UUID, logging, filesystem, types | Foundation |
| `src/project/` | Project serialization and settings | Phase 1 |
| `src/scene/` | Scene and quad management, mesh subdivision | Phase 1-2 |
| `src/renderer/` | OpenGL rendering pipeline, shaders, textures | Phase 1-2 |
| `src/media/` | Media library and image loading | Phase 1 |
| `src/video/` | Video decoding (FFmpeg) and playback | Phase 3 |
| `src/audio/` | Audio mixing and multi-output routing | Phase 6 |
| `src/output/` | Output window management, perform mode | Phase 4 |
| `src/control/` | OSC and MIDI external control (ADR-048) | Post-MVP |
| `src/source/` | Media sources: video, HAP, images, procedural, camera | Phase 3 |
| `src/editor/` | Editor state and mutations, actions, perform lock: no ImGui panels (ADR-051) | ADR-051 |
| `src/ui/` | Panels, PanelHost, EditorUi, menus and dialogs with Dear ImGui | All Phases |
| `src/input/` | Canvas key maths (`VertexKeyboard.hpp`) | Phase 7 |
| `src/app/` | Application loop, ProjectSession, AiAssist, AppActions, bench/stress | All Phases |
| `tests/` | GoogleTest unit and integration tests | All Phases |

## Code Conventions

### Naming

| Element | Style | Example |
|---------|-------|---------|
| Classes/Structs | PascalCase | `QuadGeometry`, `VideoDecoder` |
| Functions/Methods | camelCase | `getCorners()`, `loadTexture()` |
| Variables | camelCase | `meshResolution`, `currentFrame` |
| Constants | SCREAMING_SNAKE_CASE | `MAX_MESH_RESOLUTION` |
| Namespaces | lowercase | `quadmapper::scene` |
| Files | PascalCase | `QuadGeometry.hpp/.cpp` |

### Headers

- Use `#pragma once` for include guards
- Include order:
  1. Corresponding header
  2. Project headers
  3. Third-party headers
  4. Standard library
- Forward declare when possible to reduce compile times

### Error Handling

- Use `Result<T, Error>` type for functions that can fail
- Avoid exceptions in performance-critical code
- Log errors with context using the Logger class

### Memory Management

- Prefer `std::unique_ptr` for ownership
- Use `std::shared_ptr` sparingly (only when shared ownership needed)
- RAII for all resource management (OpenGL objects, file handles, etc.)

## Component Dependencies

```
core (no deps)
  ↓
project, scene, media, video, audio, control, renderer, source, output, input
  ↓
editor   (EditorState, commands helpers, ActionRegistry, PerformLock; no ImGui panels)
  ↓
ui       (panels, PanelHost, EditorUi, ShortcutRouter)
  ↓
app      (Application loop, ProjectSession, AiAssist, AppActions)
```

The `layering_check` ctest fails on any `ui/` or `app/` include under `src/editor` and any
`app/` include under `src/ui` (ADR-051).

## Testing

### Running Tests

```powershell
# Run all tests
ctest --preset debug

# Run specific test executable
./build/debug/tests/unit/scene_tests.exe

# Run with filter
./build/debug/tests/unit/scene_tests.exe --gtest_filter="QuadTest.*"
```

### Test Structure

- Mirror source structure: `src/scene/Quad.cpp` → `tests/unit/scene/QuadTest.cpp`
- Use GoogleTest fixtures for setup/teardown
- Integration tests in `tests/integration/`

## Key Files

| File | Purpose |
|------|---------|
| `src/scene/Quad.hpp` | Core quad data structure |
| `src/scene/QuadGeometry.hpp` | Corner and mesh geometry |
| `src/scene/MeshSubdivision.hpp` | Mesh generation algorithm |
| `src/renderer/Renderer.hpp` | Main rendering interface |
| `src/video/VideoDecoder.hpp` | FFmpeg video decoding |
| `src/project/ProjectSerializer.hpp` | .qmap file format |
| `src/ui/panels/InspectorPanel.hpp` | Main property editor |
| `src/editor/EditorState.hpp` | Live editor model and selection |
| `src/editor/ActionRegistry.hpp` | Editor actions: keys and menu run the same code |
| `src/ui/EditorUi.hpp` | Frame order: canvas, dockspace, menu, shortcuts, takes, panels |
| `src/app/Application.hpp` | Main loop, setup/shutdown, projector frames |

## Third-Party Libraries

| Library | Header | Purpose |
|---------|--------|---------|
| GLFW | `<GLFW/glfw3.h>` | Window and input |
| GLAD | `<glad/glad.h>` | OpenGL loader |
| GLM | `<glm/glm.hpp>` | Math library |
| Dear ImGui | `<imgui.h>` | UI framework |
| FFmpeg | `<libavcodec/avcodec.h>` | Video/audio decode |
| nlohmann/json | `<nlohmann/json.hpp>` | JSON serialization |
| stb_image | `<stb/stb_image.h>` | Image loading |
| miniaudio | `<miniaudio.h>` | Audio playback |

## Performance Guidelines

- Target 60 FPS with multiple 1080p outputs
- Video decoding on background threads
- Texture uploads on main thread only (OpenGL constraint)
- Minimize OpenGL state changes per frame
- Use texture caching aggressively

## Session Management

This project uses a session management system for development continuity:

- **`/start`** - Loads context from last session and auto-continues work
- **`/end`** - Saves progress and creates handoff notes for next session

### Tracking Files

| File | Purpose |
|------|---------|
| `PROJECT_HISTORY.md` | Chronological development log |
| `TASKS.md` | Task status tracking (QM-XXX IDs) |
| `SESSION_STATE.json` | Machine-readable state for auto-continue |

## Sub-Agent Strategy

Development uses automatic sub-agent spawning:

### Agent Naming
```
Agent-{Phase}-{Component}[-{Subcomponent}]
```

### Phase Dependencies
- **Sequential:** Phase 1 → Phase 2 → Phase 3
- **Parallel:** After Phase 3, Phases 4, 5, 6 can run in parallel
- **Final:** Phase 7 (Polish) after all others

### Spawn Triggers
Agents spawn automatically when:
- Previous phase completes
- Component interface is ready for dependent work
- Integration tests pass

Spawn-rule configuration (`.claude/agent-state/spawn-rules.json`) is not present yet.

## Common Tasks

### Adding a New Panel

1. Create `src/ui/panels/NewPanel.hpp` and `.cpp` (add both to `src/ui/CMakeLists.txt`) that
   subclass `Panel` (`src/ui/panels/Panel.hpp`): a stable `id()` (the ImGui window name), and
   `drawContent(EditorContext&)`; the host does Begin/End. Override `kind()` for a Resources tab
   or an overlay, `tick()` for work that must run while hidden, and `editsShow()` (false only for
   show controls, so the perform lock leaves it usable).
2. Register it in `EditorUi` (`src/ui/EditorUi.cpp`). The Window menu is generated from the
   registered panels, so there is no flag in `EditorState` and no menu edit.
3. Get everything through `EditorContext`; editor code that must open it posts
   `es.requestPanelOpen(id)`.
4. Check the dock layout on a first run (`imgui.ini`) so it docks sensibly.

### Adding a New Keyboard Shortcut

- **Editor-wide shortcuts** are actions in the `ActionRegistry` (`src/editor/ActionRegistry.hpp`):
  ids and default chords in `src/editor/EditActions.cpp`, registered in `src/app/AppActions.cpp`.
  Give the action default chords (several allowed; the first shows in the menu), a class
  (Show/Edit/File/View) and a guard. `ShortcutRouter` fires it on any chord with exact modifiers;
  the menu uses `actionMenuItem`. Never hand-write key checks for editor-wide keys.
- **Users rebind** in Edit > Preferences > Keyboard; overrides live in `preferences.json` under
  `"keyboard"` and are applied at startup. The registry refuses Tab, the arrows and H (any
  modifiers), another action's chord, and bare keys other than F1–F24 and Delete; F11, Esc and
  Ctrl+B are fixed. A new *default* chord needs the owner's sign-off.
- **Canvas editing keys** (vertex select and nudge) live in `ViewportEditor.cpp` and
  `src/input/VertexKeyboard.hpp`, which claim the keys with `SetKeyOwner`. Never let a new
  shortcut take Tab, the arrows, Shift or Alt from the canvas.
- **Keys on a focused projector window** go through the output windows' GLFW key callback
  (`OutputManager`), as scene takes and Ctrl+B blackout do.
- Keys that must not depend on the keyboard layout (scene takes 1–9) use physical GLFW key
  codes, not `ImGuiKey`.

### Adding a New Media Type

1. Extend `MediaItem` type enum in `src/media/MediaItem.hpp`
2. Add loader in `MediaLibrary`
3. Add thumbnail generation in `ThumbnailGenerator`
4. Update UI to display new type

## Debugging

- Debug build includes console output
- Use Logger for structured logging
- OpenGL debug output enabled in Debug builds
- Visual Studio debugger works with CMake projects (open folder)

## Reference Documents

- [QUAD_MAPPER_MVP_SPECIFICATION.md](QUAD_MAPPER_MVP_SPECIFICATION.md) - Complete technical specification
- [docs/PRD.md](docs/PRD.md) - Product Requirements Document
- [docs/EDD.md](docs/EDD.md) - Engineering Design Document

---

## Success Metric (Definition of Done)

QUADmapper succeeds when a live-event technician can, in under two minutes:

1. Launch the app, add a quad, and assign media (image/video/procedural) to it.
2. **Select a vertex and warp it precisely with the keyboard** — `Tab`/`Shift+Tab`
   cycles vertices, arrow keys nudge the selected vertex, **`Shift` = 10× nudge**,
   `Alt` = 0.1× fine nudge (the TouchDesigner-Kantan-style UX).
3. Send that quad fullscreen to a chosen projector output and see it warped correctly.

This is the gate. Feature count, test count, and coverage % are **not** the metric — this
workflow, verified working by running the app, is. Implemented in
[ViewportEditor.cpp](src/ui/panels/ViewportEditor.cpp) (vertex drag/nudge) with the keyboard
maths and key ownership in [VertexKeyboard.hpp](src/input/VertexKeyboard.hpp); never regress it.

## Model Selection Policy

- Routing lives in `~/.claude/CLAUDE.md` + the `switcher` skill — not restated here. Delegate
  through the named global agents; never the built-in `general-purpose` agent — it inherits
  the orchestrator's model and bypasses routing.
- Project specialists: **Advisor (on-demand)** — the Executor may spawn a single-question Opus subagent for
  architectural forks, hard-to-reverse decisions, or repeated stalls. Keep the prompt under
  200 words; receive one recommendation and return. See `.claude/agents/advisor.md`. Also in
  `.claude/agents/`: `audio-specialist` (`src/audio`), `cpp-executor` (cross-module C++ work
  from an agreed plan), `renderer-specialist` (`src/renderer`, `src/shaders`, `src/output`,
  the render loop), `ui-specialist` (`src/ui`, ImGui interaction), `video-specialist`
  (`src/video`, `VideoFileSource`, FFmpeg playback).
- Project exceptions: none.

## Change Boundaries

These areas are **no-edit-without-approval**; changing them is hard to reverse:

- **Build/dependency config** — `CMakeLists.txt`, `cmake/`, `CMakePresets.json`,
  `vcpkg.json` (esp. the `builtin-baseline`). New dependency = ADR + approval first.
- **Project file format** — `src/project/ProjectSerializer.*` and the `.qmap` schema.
  Breaking changes need versioned migration + backward-compat (existing `.qmap` files
  must still load).
- **Public module interfaces** in the dependency graph (`core` → `scene`/`renderer` → …) —
  changing a header that downstream modules include is cross-cutting; plan it.
- **`external/`** vendored libs (ImGui/GLAD/stb) — do not hand-edit; update via the
  documented process.
- **Secrets** — never commit credentials or keys; none belong in this repo.

Everything else: keep diffs **surgical** — touch only what the task needs, match the
surrounding style, and clean up only the mess your change introduces.

## Slash Commands

Project: `/start` `/end` (session continuity), `/build` `/run` `/test` (CMake/ctest wrappers).

Release-gate set (global, run before shipping): `/release` orchestrates
`test → security-harden → slop-clean → humanize → prod-ready → re-test` with a pass/fail
gate. Also available individually: `/prod-ready`, `/security-harden`, `/slop-clean`,
`/humanize`. **Run `/release` before any release build.**

Bootstrap additions: `/end` (extracts `LESSON-*` tags into the ledger and syncs TODO/FIXME
into `notes/TODOs.md`).

## Token & Context Rules

- `@`-mention specific files/paths; scope prompts on this large repo rather than reading broadly.
- `/context` to inspect, `/compact` (with a focused instruction) after milestones, `/cost`
  before expensive phases (e.g. the full audit).
- Don't paste big build logs — save to a file and `@`-mention; summarize first, then zoom in.
- Standard loop: **plan (plan mode) → execute (small diffs) → compact/checkpoint.** Which model runs it is global routing's call.

## Dev & Bug-Fix Discipline

- **Karpathy core:** think before coding · simplicity first · surgical changes · goal-driven execution. Verification is the binding constraint — wire "done" to a runnable check (run the app and warp a quad), not a feeling.
- **Minimalism ladder — before writing code, walk it in order:** (1) does this need to exist?
  — if not, skip it (YAGNI); (2) does the C++17 stdlib do it? (3) a feature of an
  already-linked lib (GLM / GLFW / ImGui / FFmpeg / stb / miniaudio)? (4) only then a new
  dependency — and that needs an **ADR + approval** (see Change Boundaries); (5) can it be a few
  lines? (6) then the minimum that works. Never cut validation, error handling, RAII/GL cleanup,
  or the success-metric UX to be "minimal."
- **Scope:** build the in-scope must-haves (the Success Metric workflow) first; everything else is explicitly deferred (tracked in `TASKS.md`), not "maybe." For a new panel or UI flow, validate the UI with dummy data before wiring the backend.
- **Build → test → commit, one change at a time.** One feature/fix per commit; no `wip`
  commits on `main`. Incremental, reversible steps.
- **Reviewer mode before "done".** After writing code, re-read your own diff as a senior
  reviewer intending to reject it (bugs, over-engineering, scope creep, missing tests,
  AI-slop); fix what you find, then declare done. For risky or fan-out changes, escalate to a
  **fresh-context verifier sub-agent** that grades the diff against the acceptance criteria —
  a separate context beats self-review.
- **Done means verified working** — a feature is complete only when its behavior is seen
  working (run the app) or user-confirmed. Green tests ≠ done (624 tests pass while the
  real interaction could still be broken).
- **Never build on an unverified foundation** — the Success Metric workflow is seen working before new phase work lands on top of it.
- **Acceptance criteria up front** — before building anything non-trivial, write the *checkable*
  criteria that define done (e.g. "vertex warps precisely with Tab-cycle + arrow nudge, sent
  fullscreen to the chosen output") and fix them before, not after — so the loop has a real gate
  and the verifier has something to grade.
- **Grounded progress** — before reporting progress, audit each claim against a run/test result
  from this session; if it isn't verified, say so — never report work you can't point to.
- **Bug-fixing:** reproduce first; add logging/visual feedback *before* reading lots of
  code. If not diagnosed in ~5 min, change strategy. Remove scaffolding logging after.
- **"Complete failure" = assume multiple root causes** — audit the whole chain
  (geometry → mesh → render → output), count expected vs actual at each step, fix all.
- **Zero technical debt** — delete old code (git is the backup), never comment it out; no
  dead/parallel paths. Any accepted deferral goes in `TECHNICAL_DEBT.md` with a cost.
- Fix debt the moment it appears — `DEFERRED`, `TEMPORARY` or `refactor later` in a core path (`src/core/`, `src/renderer/`, `ProjectSerializer`) means stop and fix; core work is never deferred. Refactors remove the old path in the same change. If a deadline would need debt, say so and get a decision.
- **Isolate parallel/background agent work in a git worktree** to keep `main` clean.

### Verification budget (owner decision 2026-10-02)

Testing serves the success metric; it is not the goal. Spend it in proportion to risk:

1. **No full `ctest` per change** (owner, 2026-10-03). Build, and run only the tests for the
   code you changed. The full suite runs once, near release (part of `/release`).
2. **A fresh-context review only for risky changes:** threading, the `.qmap` format, the
   perform lock, the render loop. Everything else gets the lead's own read of the diff.
3. **No mutation runs** except to settle one specific doubt, and then one or two at most.
4. **Stress loops: at most 5 rounds, no added load**, unless chasing a reported failure.
5. **Benchmarks only for changes meant to affect speed.**
6. **No speed work below what a user would notice** unless the owner reports a stall.
7. **The owner's hands-on testing comes before more automated proof.**

## Session Continuity Layer

Source of truth stays the existing `TASKS.md` / `SESSION_STATE.json` / `PROJECT_HISTORY.md`
(read by `/start` and `/end`). The bootstrap added only non-overlapping pieces:
[TECHNICAL_DEBT.md](TECHNICAL_DEBT.md), [notes/lessons.md](notes/lessons.md) (the
`LESSON-{ARCH|BUG|PERF|API|UI|TEST|BUILD}-NNN` ledger), and [notes/TODOs.md](notes/TODOs.md).

The one-page restart package is [notes/CURRENT_STATUS.md](notes/CURRENT_STATUS.md)
(rewritten at `/end`; the next session reads it, not the chat history). `/end` also writes a
dated log under [docs/sessions/](docs/sessions/) (the latest is the detailed source of truth on
conflict). Hard-to-reverse decisions get an ADR in [docs/adr/](docs/adr/) — written *before*
implementation (ADR-031+; ADR-001–030 are recorded informally in `SESSION_STATE.json`).

Tag TODOs inline as `TODO[cl]:` so `/end` can sync them. **Fix the process, not the output:** when a mistake is corrected, write the correction into this file, a rule in `.claude/rules/` or a skill so it can't recur.
