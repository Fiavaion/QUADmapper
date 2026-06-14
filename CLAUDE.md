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
| `src/ui/` | User interface with Dear ImGui | All Phases |
| `src/input/` | Input handling and keyboard shortcuts | Phase 7 |
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
project, scene, media (depend on core)
  ↓
renderer (depends on core, scene)
  ↓
video (depends on core, media)
audio (depends on core)
output (depends on core, renderer)
  ↓
ui (depends on all above)
input (depends on core)
  ↓
app (depends on all)
```

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

See `.claude/agent-state/spawn-rules.json` for full configuration.

## Common Tasks

### Adding a New Panel

1. Create `src/ui/panels/NewPanel.hpp` and `.cpp`
2. Inherit from `Panel` base class
3. Register in `UIManager::initializePanels()`
4. Add menu entry in `MainMenu`

### Adding a New Keyboard Shortcut

1. Add action enum in `src/input/KeyboardShortcuts.hpp`
2. Add default binding in `KeyboardShortcuts::initializeDefaults()`
3. Handle action in appropriate manager class

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
[Application.cpp:674-808](src/app/Application.cpp#L674-L808); never regress it.

## Model Selection Policy

The authoritative per-task rubric is the **`modelswitcher`** skill — consult it before any
multi-step plan or multi-agent fan-out. The tiers below are the fallback if it is absent.

- **Opus-class** — planning, architecture, multi-file/cross-cutting changes, agent-team
  lead, non-obvious debugging (render/timing/threading), high-risk refactors, audit
  synthesis, the `/release` gate.
- **Sonnet-class** — day-to-day feature work, routine refactors, test writing, rubric-driven
  scans (security/slop/dead-code), browser/manual-UX sweeps.
- **Haiku-class** — file listing, log triage, bulk mechanical edits.
- **Fable-class** — *opportunistic upgrade for long-horizon agentic coding* (investigate →
  patch → test → recover across many files). **Confirm availability via `/status` before
  routing to it** — it is a bonus over Opus, never a dependency. Best used for the
  remediation/fix phase, not the audit/planning phase.
- **Advisor (on-demand)** — the Executor may spawn a single-question Opus subagent for
  architectural forks, hard-to-reverse decisions, or repeated stalls. Keep the prompt under
  200 words; receive one recommendation and return. See `.claude/agents/advisor.md`.

> Note: the "Fable 10% / Opus 80% / Fable 10% by calendar" rule is **not** used — allocate
> by task shape (above), not by project timeline.

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

Everything else: keep diffs **surgical** — touch only what the task needs, match the
surrounding style, and clean up only the mess your change introduces.

## Slash Commands

Project: `/start` `/end` (session continuity), `/build` `/run` `/test` (CMake/ctest wrappers).

Release-gate set (global, run before shipping): `/release` orchestrates
`test → security-harden → slop-clean → humanize → prod-ready → re-test` with a pass/fail
gate. Also available individually: `/prod-ready`, `/security-harden`, `/slop-clean`,
`/humanize`. **Run `/release` before any release build.**

Bootstrap additions: `/lessons` (extract `LESSON-*` tags into the ledger), `/todos`
(scan code TODO/FIXME into `notes/TODOs.md`).

## Token & Context Rules

- `@`-mention specific files/paths; scope prompts on this large repo rather than reading broadly.
- `/context` to inspect, `/compact` (with a focused instruction) after milestones, `/cost`
  before expensive phases (e.g. the full audit).
- Don't paste big build logs — save to a file and `@`-mention; summarize first, then zoom in.
- Standard loop: **plan (Opus, plan mode) → execute (Sonnet, small diffs) → compact/checkpoint.**

## Dev & Bug-Fix Discipline

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
- **Isolate parallel/background agent work in a git worktree** to keep `main` clean.

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
