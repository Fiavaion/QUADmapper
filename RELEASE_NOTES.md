# QUADmapper v1.0.0 Release Notes

**Release Date:** December 20, 2025

## Overview

QUADmapper v1.0.0 is the first stable release of the professional quad-based projection mapping application. This release includes all core features for creating and managing projection-mapped content.

## Features

### Core Rendering
- Single and multi-quad rendering with 4-corner perspective warping
- Mesh subdivision (2-64 resolution) for smooth surface correction
- Per-vertex mesh editing for fine-tuned surface adjustments
- UV offset, scale, and rotation controls
- Z-order management for quad layering

### Media Support
- Image loading: PNG, JPG, TIF, BMP, TGA
- Video playback with FFmpeg (H.264, HEVC, ProRes, and more)
- Speed control (0.1x to 4.0x)
- Loop modes: Once, Loop, Ping-Pong
- Timeline synchronization for multiple videos

### Multi-Output System
- Multiple output windows with independent settings
- Monitor enumeration and assignment
- Resolution presets (720p, 1080p, 4K, and custom)
- Rotation support (0, 90, 180, 270 degrees)
- Perform Mode for fullscreen output on all monitors
- Borderless fullscreen option

### Scene System
- Multiple scenes per project
- Scene creation, duplication, and deletion
- Drag-to-reorder scenes
- Keyboard shortcuts (1-9) for quick scene switching
- Per-scene view state (pan, zoom, grid)

### Audio System
- Audio extraction from video files
- Audio mixer with miniaudio backend
- Master volume and mute controls
- Per-source volume, mute, and pan
- Multi-output audio routing

### User Interface
- Media Bin panel for content management
- Preview panel with interactive editing
- Output panel for display configuration
- Scene panel with inline rename and context menus
- Preferences dialog with customizable settings
- Keyboard shortcuts (Resolume-style defaults)

### Project Management
- .qmap project file format (JSON-based)
- Auto-save and backup support
- Backward compatibility with older project versions
- Project state tracking (dirty flag)

### Developer Features
- Undo/redo system with command merging
- Performance monitoring and profiling
- Error notification system
- Comprehensive logging

## System Requirements

### Minimum
- Windows 10 (64-bit)
- OpenGL 3.3 compatible GPU
- 4 GB RAM
- 100 MB disk space

### Recommended
- Windows 10/11 (64-bit)
- NVIDIA GeForce GTX 1060 or equivalent
- 8 GB RAM
- SSD for video content

## Known Issues

- Some audio integration tests are skipped (require test media files)
- PreferencesDialog has minor wchar_t conversion warning (non-critical)

## Dependencies

- GLFW 3.4
- GLM 1.0.2
- Dear ImGui 1.92.6 (docking branch)
- FFmpeg 7.1.2
- miniaudio 0.11.23
- nlohmann/json 3.12.0
- stb_image

## Test Coverage

- 624 automated unit and integration tests
- Manual test protocols documented in `docs/MANUAL_TEST_PROTOCOLS.md`

## Build Information

- Compiler: MSVC 19.44 (Visual Studio 2022)
- C++ Standard: C++17
- Build System: CMake 3.20+
- Package Manager: vcpkg (manifest mode)

## Getting Started

1. Extract the release package
2. Run `QUADmapper.exe`
3. Create a new project or open an existing .qmap file
4. Add quads and media content
5. Configure outputs and enter Perform Mode

## Documentation

- [Product Requirements Document](docs/PRD.md)
- [Engineering Design Document](docs/EDD.md)
- [Manual Test Protocols](docs/MANUAL_TEST_PROTOCOLS.md)
- [Development Guide](CLAUDE.md)

## Acknowledgments

QUADmapper was developed with Claude Code assistance across 6 development sessions.

---

For bug reports and feature requests, please visit the project repository.
