# 2026-11-25: macOS port, phases A to C (QM-128)

The session ran on the Mac (Apple Silicon, one built-in Retina display) on branch `feat/macos-port`, cut from `dev` at `098c712`. The lead was Fable 5.2 at high effort. The plan and the owner's decisions are in [ADR-062](../adr/0062-macos-port.md). Windows was not built in this session: the owner builds it on the PC.

## What happened

1. **Setup and survey.** Toolchain installed (Xcode clang, CMake, Ninja, vcpkg at the manifest baseline, triplet `arm64-osx`). A survey build in a throwaway worktree found what does not compile or link on macOS, and ADR-062 was written from it and accepted by the owner.
2. **Phase A: it builds and starts.** Presets `mac-debug` and `mac-release`; the FFmpeg frameworks; macOS folders (`~/Library/Application Support`, `Caches`, `Logs`); `main_mac.cpp` (single instance by file lock, abort handler, alerts); OSC on BSD sockets through a small shim in `OscServer.cpp`; MIDI through CoreMIDI (`MidiInput_mac.cpp`); a stand-in for the AI engine link; TIFF left to Windows.
3. **Phase B: it works.**
   - Open, Save and folder dialogs are the macOS panels, shown without blocking frames (`AsyncFileDialog_mac.mm`).
   - The built-in patterns are OpenGL 3.3 fragment shaders on both systems; `ComputeShader` is deleted. This closes the open decision "patterns need OpenGL 4.3".
   - Keyboard: Cmd where Windows has Ctrl (ImGui's own mapping), Option+M for mesh edit, Option+P for perform mode (F11 stays as a second chord), Cmd+B or Ctrl+B on a projector window, Mac key names in menus, tooltips and messages (`editor/KeyLabels.hpp`).
   - MP4 export encodes with `h264_videotoolbox`.
   - A projector window is raised above the macOS menu bar (`OutputWindow_mac.mm`).
   - The interface is drawn at its real size on Retina (it was doubled).
4. **Phase C: tests.** The unit and integration tests build and run on macOS. Windows-only tests are left to Windows (D3D11 interop, TIFF and COM, share-mode locks, the zero-copy reposition). Six tests assumed Windows and were made portable (key names, a driver rounding at an object's edge, a timer assumption, a random scene that depends on the standard library).

## Gate

`ctest --preset mac-debug | 1cab91e | 1788 passed, 0 failed, 6 disabled`. 69 of the passes are tests that skip themselves (their clips are named by a PC path, or they need Windows hardware).

One fresh-context review of the whole diff, fixed in one pass: a shared slot for the open file panel could leave a second dialog's panel uncancellable at quit (now one panel per dialog); a pattern's setup rebound the read framebuffer; two loosened tests now apply on macOS only. It found the Windows-facing edits safe to compile by reading, which is not a build.

## Verified by running the app on the Mac

- Add a quad, Tab and Shift+arrows nudge, Cmd+Z.
- Import a 1080p H.264 clip through the file panel, drag it on the quad: it plays, with sound decoded. The same with a HAP clip.
- Create checkerboard, gradient and plasma patterns; assign plasma to the quad.
- Option+M toggles grid warp. A corner dragged with the mouse lands where the pointer is (Retina hit-testing).
- Option+P sends the warped quad fullscreen, over the menu bar and without a display mode change; Cmd+B blacks out and back; Esc and Option+P leave.
- Cmd+S through the save panel; the show reopens with its warp and pattern.
- File > Export Video: 300 frames at 2056x1258, H.264, BT.709; a frame pulled from the file shows the warped quad.
- OSC: `/qm/blackout 1` and `0` over UDP act and are counted in the Control panel.
- MIDI: a virtual CoreMIDI source is listed, opens, and its control changes show as "Last: CC ch1 7 = …".

## Not verified

- **Fullscreen on a second display.** Only the built-in display was attached. This is the last step of the success metric and needs the owner.
- **The Windows build.** Every shared file edited here is unbuilt on Windows; see the list in `notes/CURRENT_STATUS.md`.
- Sound was not listened to (the log shows the audio stream opened and resynced).
- A real MIDI controller, and OSC from another machine through the macOS firewall.
- Real-media tests: their clips are named by a PC path and they skip on the Mac.

## Findings outside the port

- `calibrateProjector(..., Auto)` with a lens ten times out returns "points behind the projector" for about one random scene in four (10 of 40 seeds on libc++), where the test expects the pose-only fallback to succeed. The test's seed was a scene that works on MSVC. Not changed; noted for the owner.
- The media bin is empty after a show is reopened, although the quad keeps its media. This is the same on Windows (the bin is not in the `.qmap`).

## Accepted deferrals

TD-45 in [TECHNICAL_DEBT.md](../../TECHNICAL_DEBT.md): AI Assist, camera, TIFF, GPU decoding, packaging and signing, the repeated start-up sequence, PC-path test media, system chords in rebinding, Exit/Quit and the native menu bar.

## Next steps

1. **On the PC:** fetch `feat/macos-port`, configure, build and run `ctest`. Nothing on this branch has been compiled on Windows. Then compare the six built-in patterns by eye with 1.0.0, export an MP4, and check OSC still opens its port.
2. **On the Mac:** connect a projector or second display and run the success metric to the end (Option+P to that display). Listen to a clip with sound.
3. When both pass, merge `feat/macos-port` into `dev` by pull request. `master`, the tags and the 1.0.0 release are not touched from the Mac.
4. The next Mac phase is packaging: an `.app` bundle, signing with the owner's developer account and notarisation. After that, the items in TD-45.
