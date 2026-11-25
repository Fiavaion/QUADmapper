# 2026-11-25: macOS packaging and follow-ups (QM-129)

The second session of the day on the Mac, on branch `feat/macos-port`, unattended at the owner's request ("do as much work as possible", then a list of what the owner must do to get the release ready). The lead was Fable 5.2 at high effort. Decisions are in [ADR-062](../adr/0062-macos-port.md) section 9. Windows was not built: the owner builds it on the PC.

## What was done

1. **The app is built for macOS 13 and later.** vcpkg had built the libraries for the build machine's macOS (26) only. An overlay triplet (`cmake/triplets/arm64-osx.cmake`) and the Mac presets now set 13.0 for the libraries and the app. That showed one thing that does not exist before macOS 26, `std::from_chars` for `float`; the OBJ loader reads numbers with `strtof_l` on macOS.
2. **Packaging.** `packaging/build-mac-app.sh` builds Release, lays out `QUADmapper.app`, splits the debug symbols into a `.dSYM`, signs with the Developer ID certificate and the hardened runtime, and makes `QUADmapper-1.0.0-macOS-arm64.dmg`. `--notarize` sends the app and the disk image to Apple and staples both. In a bundle the program reads its files from `Contents/Resources` (`appFilesDir()`).
3. **TIFF images** open on macOS, through ImageIO. The TIFF tests now run on both systems.
4. **Video decodes on the GPU** through VideoToolbox, by the same path as D3D11VA on Windows. Frames match software decoding sample for sample (H.264, and 10-bit HEVC as P010).
5. **A decoder bug on both systems:** opening an MP4 or MOV read the first video packet for another check, and the hardware probe then started one packet late. An HEVC clip with a single keyframe fell back to software; other clips decoded a whole group of pictures before opening. The probe now starts at the first packet.
6. **Real-media tests** find their clips in the folder `QM_TEST_MEDIA` names (`tests/unit/TestMedia.hpp`). Without the variable the PC path is unchanged.
7. **Small Mac fixes:** rebinding refuses Cmd+Q, Cmd+M and Ctrl+Cmd+F; the File menu ends with "Quit QUADmapper"; the user guide says which keys differ; the FFmpeg notice covers the static macOS build.

## Gate

`QM_TEST_MEDIA=<stand-in clips> ctest --preset mac-debug | 88b87c5 | 1799 passed, 0 failed, 6 disabled`. 48 of the passes are tests that skip themselves (69 before): the rest of the PC's clips, or Windows hardware. Two commits came after the gate, both from the review: the OBJ number fix (`test_scene`: 17 OBJ tests pass) and a guard in the packaging script (the script was run again end to end).

One fresh-context review of the whole diff (`7f1cf31..88b87c5`), read-only, fixed in one pass. It found the decoder, TIFF, rebinding, packaging and test changes clean by reading, and two things: the macOS OBJ number reader accepted a number too small for a float, which `std::from_chars` refuses (fixed, with a test); the packaging script would have written an empty minimum version if it could not read one (fixed). It could not check the Windows compile.

## Verified by running the app on the Mac

- The packaged app, started from the mounted disk image: it finds its fonts, a point is selected with Tab and nudged with Shift+arrows, Option+P sends the warped quad fullscreen over the menu bar, Esc leaves.
- `codesign --verify --deep --strict` passes; the signature is Developer ID with the hardened runtime and a timestamp; the binary's minimum macOS is 13.0; the `.dSYM` matches the binary's UUID.
- A TIFF (LZW, 1200x775) imported through the file panel shows the right way up on a quad, with the same pixels as the same picture as PNG.
- A 4K HEVC clip and a 1080p H.264 clip with sound play on a quad with "hardware decoding" in the log, loop without a warning, at about 8% of one core.

## Not verified

- **Notarisation.** Not run: it needs the owner's notary credential. Until then Gatekeeper refuses the app on any other Mac (`spctl` says "Unnotarized Developer ID", as expected).
- **The app on macOS 13 to 15.** Built for 13; run on 26 only.
- **The Windows build**, as before, now with more shared files edited: see `notes/CURRENT_STATUS.md`.
- **Fullscreen on a second display**, sound by ear, a real MIDI controller, OSC from another machine: unchanged from the first session.
- The real-media tests ran against stand-in clips made with ffmpeg on the Mac, not the PC's clips.

## Left alone

- Camera sources: the system asks the person at the Mac for camera access, and nobody was there to answer.
- HAP R: no clip to test with. It still shows black with a line in the log.
- AI Assist, a native menu bar, the repeated start-up sequence: unchanged, in TD-45.

## Next steps

See the owner's list in `notes/CURRENT_STATUS.md`.
