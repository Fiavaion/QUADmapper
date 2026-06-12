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
