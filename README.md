# QUADmapper

QUADmapper is a projection mapping application for Windows. It warps images, video and generated patterns onto physical surfaces, so a projector's picture lines up with a wall, a screen or a stage set.

It is made for live-event technicians, VJs, installation artists and projection designers who need to line up a projector quickly and keep the show running.

## What it does

- Corner-pin and mesh warping. Pull a quad's four corners, or subdivide it into a mesh for curved surfaces. Pick a corner with `Tab` and nudge it with the arrow keys (`Shift` = 10x, `Alt` = 0.1x), or drag it with the mouse.
- Images, video, HAP video and procedural patterns (solid colour, gradient, checkerboard, grid, test patterns, noise, plasma) as sources. Video can be converted to HAP from the media bin. A webcam can be used as a source.
- Several scenes, put on air with the `1` to `9` keys, with optional crossfades between them.
- Several projector outputs. Each output can show part of the scene (slices), with edge blending where projectors overlap.
- Perform mode with blackout (`Ctrl+B`) and a perform lock (`Ctrl+Shift+L`) that blocks edits during a show.
- Projector calibration against 3D objects loaded from OBJ files.
- OSC and MIDI control, including MIDI learn.
- Autosave and crash recovery.
- Optional AI assist for surface detection and camera scanning. See `resources/docs/ai-features-guide.html`.

## System requirements

- Windows 10 or 11, 64-bit.
- A graphics card with OpenGL 3.3 or later. Every built-in pattern (solid colour, gradient, checkerboard, grid, noise, plasma and the test patterns) needs OpenGL 4.3; images and video do not.
- For the AI features: an NVIDIA graphics card for speed. Without one, the AI features run on the CPU and are much slower.

## Download

The Windows installer, `QUADmapper-Setup-<version>.exe`, is on the [Releases](https://github.com/Fiavaion/QUADmapper/releases) page. The installer is not code-signed yet, so Windows may show "Windows protected your PC" when you run it: click **More info**, then **Run anyway**.

## Getting started

Open `resources/docs/quick-start.html` in a web browser. It takes about two minutes to read and covers the first mapping, the show controls and the keyboard shortcuts. The AI features have their own guide: `resources/docs/ai-features-guide.html`.

## Building from source

You need Visual Studio 2022 with the C++ workload, CMake 3.20 or later, and vcpkg installed at `C:\vcpkg` (the path `CMakePresets.json` expects).

```powershell
# Clone the repository
git clone https://github.com/Fiavaion/QUADmapper.git
cd QUADmapper

# Configure (first time)
cmake --preset debug

# Build
cmake --build build/debug

# Run the tests
ctest --preset debug

# Run
./build/debug/src/app/Debug/QUADmapper.exe
```

### Build presets

| Preset | Purpose |
|--------|---------|
| `debug` | Development with tests and debug symbols |
| `release` | Optimised distribution build |
| `relwithdebinfo` | Profiling and production debugging |

### Project structure

```
src/
├── app/        # Application loop, project session, AI assist
├── core/       # Utilities, types, logging
├── project/    # Save/load, serialization
├── scene/      # Quads, groups, mesh subdivision
├── renderer/   # OpenGL rendering pipeline
├── media/      # Image loading, media library
├── video/      # FFmpeg video decoding
├── audio/      # Audio mixing (miniaudio)
├── output/     # Multi-window, perform mode
├── control/    # OSC and MIDI control
├── source/     # Media sources: video, HAP, images, procedural, camera
├── editor/     # Editor state, actions, perform lock
├── ui/         # Dear ImGui interface
└── input/      # Keyboard shortcuts, mouse
```

## Documentation

- [Quick start](resources/docs/quick-start.html)
- [AI features guide](resources/docs/ai-features-guide.html)
- [Release notes](RELEASE_NOTES.md)
- [Third-party notices](THIRD-PARTY-NOTICES.md)
- [Product Requirements](docs/PRD.md)
- [Engineering Design](docs/EDD.md)
- [Development Guide](CLAUDE.md)
- [Specification](QUAD_MAPPER_MVP_SPECIFICATION.md)

## License

MIT License - See LICENSE file for details.
