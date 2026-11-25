# QUADmapper Release Notes

## macOS version (in preparation)

Not released yet: the disk image is built and signed but not notarised, and which release it joins is not decided.

- QUADmapper runs on Macs with Apple Silicon, on macOS 13 or later. It comes as a disk image: open it and drag QUADmapper to Applications.
- Everything in 1.0.0 below is there, except the AI features, webcam sources and HAP R clips.
- Keys follow the Mac: `Cmd` where Windows has `Ctrl`, `Option` where it has `Alt`. Perform mode is `Option+P` and mesh edit is `Option+M`. The menus and tooltips show the Mac keys.
- Open, Save and folder choices use the macOS panels. Settings are in `~/Library/Application Support/QUADmapper` and the log in `~/Library/Logs/QUADmapper`.
- H.264 and HEVC video decode on the graphics chip (VideoToolbox), and MP4 export encodes with it.
- MIDI uses CoreMIDI, so any controller macOS sees can be used.
- A projector window covers the menu bar.

## 1.0.0

The first public release. The date is set when the release is made. It covers everything since the December 2025 development milestone further down.

### Mapping

- Each quad has a Warp choice in Properties. Linear, the default for new quads, lines up quads that are snapped together across their shared edge. Perspective makes the corner pin perspective-correct: textures follow a tilted quad exactly, with no visible bend along the diagonal. Shows saved before this choice existed open in Perspective. Project files are now format 3.5; an older build opens them with every quad in Perspective.
- Select a corner or mesh point with `Tab` / `Shift+Tab` and move it with the arrow keys. `Shift` moves ten times as far, `Alt` a tenth as far. Dragging with the mouse still works.
- The mesh grid can have uneven spacing. Add or remove grid divisions in the Properties panel, or click an edge to insert one. Grid lines have curve handles (`H` and `Shift+H` cycle them).
- Grid warp in both modes: tick Grid warp on a quad and its points show on the canvas. Add Column and Add Row put a line in the middle of the widest column or row. Click-add also works on the quad's edge, and any grid line can be dragged to a new place.
- Grid points are Spline (smooth) or Linear (straight lines between points). Choose the type for new lines beside Click-add, right-click a point to change it, or change all points at once.
- Image: Crop keeps the picture flat on the four corners and uses the bent outline to cut a shape out of it; Image: Warp (the default) bends the picture with the grid. Project files are now format 3.6; an older build opens them with every point Spline and every quad Warp.
- New quads start with the test grid on them, the first quad included. Test Grid: On / Off in the menu bar turns that off, and the choice is remembered.
- Crop insets can be shown in percent or in source pixels.
- Other scenes: On shows the other scenes' quads as outlines on the canvas, and dragged points snap to them, for two projectors stacked on one surface.
- Mesh editing has its own keys: `Ctrl+M` toggles mesh edit, `Alt+=` and `Alt+-` change the mesh resolution, `Alt+R` resets the mesh.
- Each quad can have a crop rectangle, colour correction and a soft edge.
- Quads can be grouped, locked and hidden from the Quad List. A locked quad cannot be reshaped or deleted until you unlock it.
- 3D objects: import an OBJ file (File > Import 3D Object (OBJ)...), and calibrate a projector against it by clicking known points on the model and on the canvas (Edit > Calibrate Projector...). Calibration works on any output, however it is sliced.
- Images now load the right way up. A saved show that used Flip V as a workaround needs Flip V turned off.
- Undo and redo cover deleting and duplicating quads, 3D objects and calibration points. Redo is on `Ctrl+Y` and `Ctrl+Shift+Z`.
- Zoom and pan the canvas; the interface has a Simple mode and an Advanced mode.
- A new look: a refined colour theme with the Hanken Grotesk and JetBrains Mono fonts.

### Media and playback

- HAP and HAP Q video play back, and "Convert to HAP" in the media bin converts other video files.
- A video is decoded once and can be used by many quads.
- Video playback uses the graphics card for decoding and colour conversion where it can, so more and larger videos play smoothly.
- Playback follows the clock: loops wrap without a hitch, Ping-Pong plays backwards properly, and a master timeline lets several videos start and stop together ("Lock to master timeline").
- Sound from video files, kept in step with the picture, with a short crossfade at each loop point.
- TIF and TIFF images load.
- A webcam can be used as a source.
- Procedural sources: solid colour, gradient, checkerboard, grid, SMPTE bars, noise and plasma.
- File names with non-English characters work everywhere.
- Missing media is kept when a project loads. It shows black on the projectors and a magenta check in the editor. Use File > Relocate Missing Media... to find it again.

### Outputs and show control

- A project holds several scenes. Each scene is a canvas that any number of outputs can show.
- Press `1` to `9` to put a scene on air, in the editor or on a focused projector window. Scenes can be duplicated and reordered.
- Scene takes can crossfade. Set the default fade time in Edit > Preferences... and use 0 for a cut. Sound crossfades with the picture.
- Projectors are found by themselves. At startup and on File > New, every screen gets an output at its own resolution and a scene of its own (Scene 1 on Projector 1, Scene 2 on Projector 2). The editor's screen comes last, and in perform mode its output covers the editor; untick it to keep the editor in view. A projector plugged in while the app runs gets one too, except during perform mode or the perform lock. Choosing a screen for an output in the Outputs tab now sets its resolution, as Properties already did.
- Each output can show slices of the canvas, each slice with its own warp. Edge blending, with width, gamma and curve, joins overlapping projectors. Auto-blend sets the blend on edges that face each other. A blend test pattern helps check the overlap.
- With several displays at different refresh rates, the show is paced by the slowest one.
- File > Export Video... renders the show to MP4 files (H.264, with the videos' sound), one per output at its own resolution and rotation, to play from a media player without the PC. The length is the video's own when only one video is mapped, or a length you set. Every frame is rendered, however busy the PC is.
- Perform mode (`F11`) sends the show to the projectors. `Esc` or `F11` leaves it.
- Blackout (`Ctrl+B`) turns every projector black at once.
- Perform lock (`Ctrl+Shift+L`, or Output > Perform Lock) blocks edits during a show and keeps show controls working. Unlocking asks for confirmation.
- Keyboard shortcuts can be changed in Edit > Preferences... > Keyboard. Redo and several other actions accept more than one key.
- A Help menu, a status bar and a remembered panel layout (Window > Reset Layout restores the default).
- `Ctrl+G` groups the selected quads (also Edit > Group Selected).
- File > Open Recent lists the last ten shows.
- Edit > Preferences... > Interface size makes the interface larger or smaller than the monitor's own scale.
- The program has its own icon, and Explorer shows its version.

### External control

- OSC input (off until you turn it on in the Control window, default port 8000) and MIDI input with MIDI learn.
- OSC feedback to a controller, OSC address patterns with wildcards, and 14-bit MIDI controllers.
- MIDI mappings are saved in the project.
- Compositions: a Timeline plays cues (videos, images, generators) one after another with a crossfade; a Switcher changes cue on Next, Previous or Cue n. Put one on any quad from the Compositions tab, and drive it by OSC (`/qm/composition/<name>/next`, `/prev`, `/cue`, `/play`, `/pause`, `/restart`, `/fade`) or MIDI learn. Project files are now format 3.7; an older build opens them with those quads empty.

### AI assist

AI assist is a beta and is off until you turn it on in Edit > Preferences... > General > AI Assist (beta). Mapping does not need it. It needs a one-off setup and a webcam. See the AI features guide.

- Detect Surface proposes a quad from a camera photo.
- Pick Surface: click a surface in a photo and the app outlines it and proposes the corners.
- Scan Output: the projector shows stripe patterns and the camera maps itself to the projector, so corners land precisely, even on surfaces that are not flat.
- One-Flash: a single projected pattern and one photo give a flat-surface placement, and an anamorphic warp for a chosen viewpoint.
- Place From Photo: click calibration points in the photo instead of placing them by hand.
- AI requests run in the background, so the editor stays usable. Every AI warp is one undo step.

### Reliability

- Saves are written safely, so a crash or power cut cannot leave a half-written project. Autosave writes a recovery file (every minute by default); after a crash, QUADmapper offers to restore it.
- Unsaved changes are checked before New, Open or exit.
- Opening, closing and moving through videos no longer freezes the interface.
- Outputs, media and audio were checked with repeated perform-mode cycles for leaks and hangs.
- Undo covers the mesh keys and every edit in the Properties panel, one step per drag.
- Deleting a scene, removing an output and removing media that quads use ask first.
- Status and error messages show in the status bar in both modes. A failed start and a fatal error show a message.
- QUADmapper keeps a log, and a crash report after a crash (Help > Open Log Folder). After a crash it tries to save a recovery copy of the show.
- Drop a `.qmap` file on the window to open it. The window title shows the open show and whether it has unsaved changes.
- The app no longer depends on the folder it was started from, and its settings, layout and logs are kept in the user's own folders.
- Greyscale images show in grey (they showed red).

## Known limitations

- Displays at different refresh rates: the show runs at the rate of the slowest display. A faster display repeats frames. An output in exclusive fullscreen at a different rate may tear.
- No MIDI Timecode or MIDI clock sync. OSC bundle time tags are ignored. OSC feedback and 14-bit MIDI have not been tried with real controllers.
- Audio delay is estimated, not measured, and the delay of the projector is not compensated. Unplugging or switching an audio device has not been tested on real hardware.
- Under heavy load, opening or removing a video can still cause a short pause of up to about 0.05 seconds.
- A Scan Output map can be wrong when an output runs at an exclusive-fullscreen mode that is not the desktop mode. If a warp looks off, scan again.
- On outputs with slices or rotation, Pick Surface and Detect Surface place corners only approximately. Fix them with `Tab` and the arrow keys.
- The settle time between scan patterns is not tuned for real webcams. A slow webcam can give a low "camera saw N%" figure.
- Detect Surface was tuned on three test images. On some venues it may propose a wrong or low-confidence surface. Use Pick Surface instead.
- Open projects only from people you trust. A project can point at media on a network share (`\\computer\share\...`), and opening it makes Windows connect to that computer and sign in with your Windows account.

---

# Development milestone, December 2025 (not published)

These notes were written for an internal build that was numbered 1.0.0 but never released. The public 1.0.0 is the section at the top.

**Date:** December 20, 2025

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

- GLFW 3.5.1
- GLM 1.0.3
- Dear ImGui 1.92.6 (docking branch)
- FFmpeg 9.0.2
- miniaudio 0.11.25
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
