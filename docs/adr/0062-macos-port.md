# ADR-062: A macOS port, starting with the success metric on a Mac

**Date:** 2026-11-25
**Status:** Accepted (owner, 2026-11-25, with the decisions in section 8)
**Deciders:** Jones
**Relates:** ADR-044 (HAP), ADR-046 (recovery and paths), ADR-048 (external control), ADR-051 (layering), ADR-053 (AI engine), ADR-056 (GPU video frames), ADR-060 (video export)

## Context

- QUADmapper has only been built with MSVC on Windows. The owner wants a macOS version.
- A survey build was made on 2026-11-25 on an Apple Silicon Mac (macOS 26.6, Apple clang 21, CMake 4.4.4, Ninja, vcpkg at the pinned baseline, triplet `arm64-osx`). Nothing from it is committed. Its results are in section 7.
- Short version of the survey: the port is smaller than the Windows-only list suggested.
  - All vcpkg packages build unchanged. FFmpeg 9.0.2 comes with VideoToolbox and AVFoundation.
  - With three small throwaway edits, 118 of 126 source files compile. The 8 that do not are Windows API files by nature.
  - OpenGL already works: the renderer and the projector-output tests ran on the Mac and 157 of 159 passed.
  - The Mac gives OpenGL 4.1 with GLSL 4.10. It has no compute shaders, so the six built-in patterns cannot run as they are.
- The Windows 1.0.0 release is finished on the work PC. The port must not disturb it.

## Decision

### 1. Scope of the first Mac milestone

The milestone is the success metric from `CLAUDE.md`, done on a Mac by hand:

1. Launch the app, add a quad, assign an image, a software-decoded video and a built-in pattern.
2. Select a vertex with Tab and Shift+Tab, nudge with the arrows, Shift for 10×, Option (Alt) for 0.1×.
3. Send the quad fullscreen to a second display and see it warped correctly.
4. Save the show, quit, reopen it.

It is a development build started from the build folder. It is not a signed `.app` and not an installer.

In the milestone:

- The editor, the canvas, quads, mesh and grid warp, scenes and takes, perform mode, undo.
- Images through stb (PNG, JPEG and the rest of its formats).
- Video with software decoding, with sound through miniaudio (CoreAudio).
- HAP, HAP Alpha and HAP Q. The Mac reports `GL_EXT_texture_compression_s3tc`.
- The six built-in patterns, rewritten (section 3).
- Open and Save dialogs (section 4), and drag and drop, which already works through GLFW.
- OSC. `OscServer.cpp` uses Winsock calls that have direct BSD socket equivalents; it is about 230 lines.
- MIDI input through CoreMIDI (owner decision: in the first milestone).
- Video export to MP4 with the `h264_videotoolbox` encoder (owner decision: in the first milestone).
- Preferences, the log, autosave and recovery in the Mac's standard folders.

Deferred, each with a stub that compiles and says "not available on macOS yet" where the user would meet it:

| Deferred | Why | Stub |
|---|---|---|
| AI Assist (the engine link) | A beta, off by default; WinHTTP, `CreateProcessW` and a Job Object | The beta switch is hidden on the Mac |
| Camera sources | AVFoundation through libavdevice needs a camera permission and an `Info.plist` | No devices listed |
| GPU video decoding and zero-copy | VideoToolbox; software decoding is enough for 1080p on Apple Silicon | Already falls back |
| TIFF | Windows uses WIC; the Mac would use ImageIO | A clear "TIFF is not supported on macOS yet" error |
| HAP R (BC7) | The Mac's OpenGL has no `GL_ARB_texture_compression_bptc` | Already capability-gated |
| Crash dump, single-instance check | macOS writes its own crash reports; a single instance comes with an `.app` bundle | None needed |
| `.app` bundle, icon, signing, notarisation, installer | A packaging phase of its own | None |
| The PowerShell smoke harness | Windows-only scripts | None |

### 2. How platform code is separated

- **One header, one source file per platform.** A module that needs the operating system keeps its header as it is and gets `Thing_win.cpp` and `Thing_mac.cpp` (or `.mm`). CMake picks one. New code gets no `#ifdef` forests.
- **Existing small `#ifdef _WIN32` blocks stay** where the two branches are a few lines (`Logger.cpp`, `MediaRelocation.hpp`). The large ones (`DxInterop.cpp`, the D3D11 part of `VideoDecoder.cpp`) already compile on the Mac and are left alone.
- **Folder lookups go through `core/FileSystem`** only. Today four places find `%APPDATA%` themselves (`FileSystem.cpp`, `Preferences.cpp`, `ProjectSession.cpp`, `ThumbnailCache.cpp`). On the Mac:
  - settings and recovery files: `~/Library/Application Support/QUADmapper`
  - thumbnails: `~/Library/Caches/QUADmapper` (macOS may empty this folder, which is fine for a cache and not for recovery files)
  - logs: `~/Library/Logs/QUADmapper`
- **Small shell actions** (open a URL, reveal a file, the process id, memory in use) move into one `core/Platform` pair of files.
- **`main.cpp` is split:** the shared start-up stays; the crash filter, the single-instance mutex, `WinMain` and the DLL search path go to `main_win.cpp`.
- **Windows behaviour does not change.** Each move is a mechanical one, and the Windows build and tests must pass on every commit (section 6).

### 3. OpenGL

- **Stay on OpenGL for the port.** The context request (3.3 core, forward-compatible) is already what macOS needs, and it returns 4.1. Metal would be a rewrite of the renderer.
- **The patterns move from compute shaders to fragment shaders, on both platforms.** Each of the six `#version 430` compute shaders becomes a `#version 330` fragment shader drawn as one fullscreen triangle into the pattern's own framebuffer. `CompositionSource` already draws this way.
  - `ComputeShader.hpp/.cpp` and the runtime lookup of OpenGL 4.3 functions are deleted. There is one path, not two.
  - This also settles open owner decision 2 from Windows (review item M22): the app then needs only the OpenGL 3.3 it asks for.
  - The pictures must not change. The old compute path cannot run on a Mac, so no before-and-after comparison was possible there: `ProceduralPatternRenderTest` reads each pattern back and checks known values (the solid colour, the checker cells, the gradient's ends, places in the test patterns, that noise and plasma repeat and animate) and that drawing a pattern leaves the GL state as it found it. On the Windows build the owner compares the patterns by eye against 1.0.0.
- **Retina.** The editor window's framebuffer is twice its window size on the built-in display. The canvas maths that mixes window and framebuffer sizes (`ViewportEditor.cpp`, `Application.cpp`, `OutputManager.cpp`) is checked by running the app, and fixed where the hit-testing or the viewport is off.
  - Found by running the app: the fonts were scaled by the display's content scale, which on macOS doubles an interface already sized in points. The scale is 1 on macOS. Canvas hit-testing needed no change.
- **Projector windows.** macOS draws its menu bar on every display, above an ordinary borderless window. The borderless output is raised above the menu bar and the Dock (`OutputWindow_mac.mm`). GLFW's own fullscreen was tried and rejected: on a display in a scaled mode it changes the display's mode.
- **Limits.** The Mac's largest texture is 16384 a side. `Texture.cpp` already checks the real limit.
- `GL_SILENCE_DEPRECATION` is defined on Apple so the build log stays readable.

### 4. File dialogs

- `AsyncFileDialog` returns "nothing chosen" on anything but Windows, so on a Mac nothing can be opened or saved through a dialog today.
- The Mac gets `AsyncFileDialog_mac.mm`: an `NSOpenPanel` or `NSSavePanel` opened without blocking, with the result handed back through the same flag the panels already poll. Projector frames keep running while it is open, as on Windows.
- This needs Objective-C++ enabled in CMake for Apple (`OBJCXX`) and the AppKit framework. It is **not** a new dependency: both are part of the system. It is the only Objective-C++ file in the milestone.

### 5. Keyboard

- Tab, Shift+Tab, the arrows, Shift and Alt (Option) work unchanged. They are read as physical keys.
- Dear ImGui swaps Ctrl and Cmd on a Mac by itself, so every editor chord defined with Ctrl (`Ctrl+Z`, `Ctrl+S`, `Ctrl+,` for Preferences) already answers to Cmd. Menus must say Cmd, not Ctrl.
- Three things do not carry over. The owner decided each on 2026-11-25; they apply to the Mac only, and the Windows defaults do not change:
  - `Ctrl+M` (Toggle Mesh Edit) would become Cmd+M, which macOS uses to minimise the window. On the Mac it is **Option+M**, beside the other mesh keys (Option+=, Option+-, Option+R).
  - `F11` (perform mode) is the volume-down key on a Mac keyboard. On the Mac perform mode is **Option+P**, and the menu shows that.
  - On a focused projector window the blackout chord is read by GLFW as the physical Control key. On the Mac a projector window accepts both **Cmd+B** and Control+B.

### 6. Branch, commits and the Windows build

- Work happens on `feat/macos-port`, off `dev`. Nothing is pushed until the owner asks. `master`, tags and releases are not touched from the Mac.
- One change per commit. The Windows build cannot be run on the Mac. The branch is pushed at the end of each phase and the owner builds and tests it on the work PC (decision 7).
- The port is not merged to `dev` until the Windows full suite has passed on it.

### 7. Change-boundary edits that need approval

No new dependency, no change to `vcpkg.json`'s packages or baseline, no change to the `.qmap` format, no edit in `external/`.

| File | Edit |
|---|---|
| Root `CMakeLists.txt` | Guard the MSVC-only install lines (the PDB install stops CMake generating on a Mac). Enable `OBJCXX` on Apple. Set the deployment target and `GL_SILENCE_DEPRECATION` |
| `CMakePresets.json` | Add `mac-debug` and `mac-release`. The Windows presets stay as they are |
| `src/ai`, `src/control`, `src/media`, `src/app`, `tests/unit` build files | `winhttp`, `winmm`, `ws2_32`, `windowscodecs`, `ole32`, `Psapi` only on Windows; the per-platform source lists |
| `src/video`, `src/audio` build files | Link FFmpeg through the imported pkg-config target. On the Mac FFmpeg is a static library and needs the VideoToolbox, CoreMedia, CoreVideo, AudioToolbox, AVFoundation and CoreServices frameworks, which the current `find_library` lines do not bring |
| `src/app/CMakeLists.txt` | Copy `resources/` beside the executable on every platform (today only inside `if(WIN32)`) |
| `src/ui/CMakeLists.txt` | The `.mm` file and AppKit |
| Public headers | `core/FileSystem.hpp` (cache and log folders, executable folder on the Mac), a new `core/Platform.hpp`, `core/MainThreadTimings.hpp` (`<process.h>`), `renderer/ComputeShader.hpp` removed, `source/procedural/ProceduralSource.hpp` |
| Default key chords | Only what decision 4 settles |

## Survey record

`cmake --preset mac-debug` (a user preset), then `cmake --build build/mac-debug -- -k 0` | `098c712` plus three uncommitted survey edits | 118 of 126 source files compiled.

The three survey edits, all thrown away: the PDB install line guarded in the root `CMakeLists.txt`; `<process.h>` and `_getpid` in `MainThreadTimings.hpp` (it hid errors in 9 files); one call to a D3D11-only function in `VideoDecoder.cpp:824` that sits outside its `#ifdef`.

| Module | Compiled | What fails and why |
|---|---|---|
| core, project, scene, renderer, video, audio, output, source, editor, ui, input | all | Nothing, after the two source edits above |
| media | 3 of 5 | `ImageLoader.cpp`: `<windows.h>` and WIC for TIFF. `ThumbnailCache.cpp`: `_wdupenv_s` to find `%APPDATA%` |
| control | 9 of 11 | `MidiInput.cpp`: WinMM. `OscServer.cpp`: Winsock |
| ai | 0 of 1 | `SidecarClient.cpp`: Winsock, WinHTTP, process and Job Object calls |
| app | 6 of 9 | `main.cpp`: crash filter, mutex, `WinMain`. `ProjectSession.cpp`: `SHGetKnownFolderPath`. `StressHarness.cpp`: `psapi` |
| tests | 13 files | `<process.h>` or `_getpid` (5), Winsock in the OSC and engine tests (3), `<windows.h>` (4), `_putenv_s` (1). `DxInteropTest` is Windows-only by design |

The app was not linked or started: `media`, `control`, `ai` and `app` do not build yet.

Tests that built and ran on the Mac:

| Test program | Result |
|---|---|
| `test_input` | 15 passed |
| `test_project` | 109 passed |
| `test_scene` | 391 passed, 1 failed: `ProjectorCalibrationTest.AutoNeverAppliesAnImplausibleLens`. The solve gives a different answer on arm64. Turning off fused multiply-add did not change it; the cause is not found yet |
| `test_renderer` | 7 passed |
| `test_output` | 146 passed, 2 failed: `RenderOnceReadbackTest.OutputPixelsMatchTheDirectPath` at 0° and 180°, 1 pixel of 61,440 differs |
| `test_integration_rendering` | 4 passed |

What the Mac's OpenGL reports: version 4.1, GLSL 4.10, largest texture 16384, S3TC yes, BPTC no, compute shaders no, debug output no. The built-in display has a scale of 2.

Not checked, because it needs the running app or hardware: fullscreen on a second display (only the built-in display was attached), "Displays have separate Spaces", Retina hit-testing on the canvas, video playback speed, sound.

## Consequences

- One codebase builds on both systems. The Windows release is unaffected until the port branch merges.
- The pattern rewrite touches Windows too. It removes the OpenGL 4.3 requirement and a hand-rolled function loader, at the cost of re-verifying six pictures.
- A Mac build exists that cannot yet be handed to other people. Gatekeeper blocks an unsigned app on another Mac; that waits for the packaging phase and an Apple Developer ID.
- Shows are not yet portable between the systems: a `.qmap` holding absolute Windows paths (`D:\...`) will open on a Mac with its media missing. The existing "missing media" search covers it. A proper answer is deferred.
- FFmpeg is linked statically on the Mac and as DLLs on Windows. For a distributed build the LGPL is easier to satisfy with dynamic libraries; that choice belongs to the packaging phase.
- macOS has marked OpenGL as deprecated since 2018. It still works on macOS 26. If Apple removes it, the renderer needs a Metal path or a translation layer; that is a later ADR.

## 8. Owner decisions (2026-11-25)

| # | Decision | Decided |
|---|---|---|
| 1 | Architecture | Apple Silicon only |
| 2 | Rewrite the patterns for OpenGL 3.3, on Windows too | Yes |
| 3 | Graphics API | OpenGL |
| 4 | Keyboard on the Mac | Cmd for the editor chords; Option+M for Toggle Mesh Edit; Option+P for perform mode; Cmd+B and Control+B for blackout on a projector window |
| 5 | The first milestone | As section 1: OSC, MIDI and MP4 export are in; AI Assist, camera, GPU decoding, TIFF and packaging are deferred |
| 6 | Apple Developer ID | The owner has one; packaging is still its own later phase |
| 7 | The Windows build | Built and tested by the owner on the work PC from the pushed branch |
| 8 | File dialogs | The system panels, through one Objective-C++ file |

The oldest macOS supported is not fixed yet. The development build targets the macOS it is built on; the packaging phase sets the minimum.
