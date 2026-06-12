# QUADmapper

Professional quad-based projection mapping application for Windows 11.

## Features

- **Multi-quad warping** - Map content to complex surfaces with 4-corner perspective transform
- **Mesh subdivision** - Smooth curved surface mapping with up to 64x64 mesh
- **Multiple outputs** - Drive unlimited projectors with independent resolution/rotation
- **Scene system** - Instant content switching with keyboard shortcuts (1-9)
- **Video playback** - Multiple video formats with timeline sync and loop modes
- **Audio mixing** - Per-source volume control with multi-output routing

## Requirements

- Windows 10/11 (64-bit)
- Visual Studio 2022 with C++ workload
- CMake 3.20+
- vcpkg with VCPKG_ROOT environment variable set
- GPU with OpenGL 3.3+ support

## Quick Start

```powershell
# Clone the repository
git clone https://github.com/youruser/quadmapper.git
cd quadmapper

# Configure (first time)
cmake --preset debug

# Build
cmake --build build/debug

# Run
./build/debug/QUADmapper.exe
```

## Build Presets

| Preset | Purpose |
|--------|---------|
| `debug` | Development with tests and debug symbols |
| `release` | Optimized distribution build |
| `relwithdebinfo` | Profiling and production debugging |

## Project Structure

```
src/
├── app/        # Application entry point
├── core/       # Utilities, types, logging
├── project/    # Save/load, serialization
├── scene/      # Quads, groups, mesh subdivision
├── renderer/   # OpenGL rendering pipeline
├── media/      # Image loading, media library
├── video/      # FFmpeg video decoding
├── audio/      # Audio mixing (miniaudio)
├── output/     # Multi-window, perform mode
├── ui/         # Dear ImGui interface
└── input/      # Keyboard shortcuts, mouse
```

## Documentation

- [Product Requirements](docs/PRD.md)
- [Engineering Design](docs/EDD.md)
- [Development Guide](CLAUDE.md)
- [Specification](QUAD_MAPPER_MVP_SPECIFICATION.md)

## License

MIT License - See LICENSE file for details.
