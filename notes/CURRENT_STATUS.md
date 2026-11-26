# Current Status

**Last updated:** 2026-11-25 (Windows: the owner's hands-on test passed, release steps under way. Mac: port and packaging done on `feat/macos-port`, disk image notarised 2026-11-26; the Windows build of that branch is the owner's)
**Phase / focus:** Release 1.0.0. The owner tested the app on 2026-11-25 and is happy to move forward. Now: merge to `master`, build the installer there, then replace the published 1.0.0 when the owner says publish.
**Dev:** `494cc40` plus notes commits. 94 code commits since the 2026-11-23 status note (QM-127).

**Mac (2026-11-25, QM-128 and QM-129):** the port and its packaging are done on branch `feat/macos-port` (off `dev` at `098c712`, pushed), on the Mac. There is a signed `QUADmapper.app` and disk image for Apple Silicon, macOS 13 or later; it was **notarised by Apple on 2026-11-26** and both tickets are stapled (`spctl`: "Notarized Developer ID"). **Nothing on that branch has been compiled on Windows.** Plan and decisions: [ADR-062](../docs/adr/0062-macos-port.md) (section 9 for packaging). Records: [the port](../docs/sessions/2026-11-25-macos-port.md), [packaging](../docs/sessions/2026-11-25-macos-packaging.md). The Windows 1.0.0 release is finished on the work PC and is not touched from the Mac.

## Mac: where it stands

- **Gate (Mac):** `QM_TEST_MEDIA=<stand-in clips> ctest --preset mac-debug | 88b87c5 | 1799 passed, 0 failed, 6 disabled`, then two review fixes with targeted checks (`9a3a4df`, `64e6688`). 48 of the passes are tests that skip themselves. Windows-only tests are not built on macOS (`test_ai`, the D3D11 interop and COM cases).
- **Works, seen in the running app:** everything in the port's record, and from the packaged app started off the disk image: Tab and arrow nudge, Option+P fullscreen, a TIFF on a quad, 4K HEVC and 1080p H.264 with hardware decoding (VideoToolbox).
- **Not there on the Mac:** AI Assist, camera sources, HAP R. See TD-45.

## What the owner has to do for the Mac release

1. **On the PC: build `feat/macos-port` and run `ctest`.** Shared files edited on the Mac without a Windows build:
   - From the port: `OscServer.cpp` (socket shim), `ProjectSession.cpp` and `FileSystem` (folders), `ThumbnailCache.cpp`, `MediaBinPanel.cpp`, the panels (key names through `editor/KeyLabels.hpp`), `AsyncFileDialog.cpp`, `Mp4Writer.cpp` (pixel format choice), `OutputWindow.cpp`, `Application.cpp`, `ProceduralSource` and the six patterns (now 3.3 fragment shaders: compare by eye with 1.0.0), every `CMakeLists.txt` with an `if(WIN32)` or `if(APPLE)`, and the tests. `main.cpp`, `MidiInput.cpp` and `SidecarClient.cpp` were renamed `*_win.cpp` with no change inside.
   - From packaging: `VideoDecoder.cpp` (the decoder is chosen per system, and **the hardware probe at open now seeks to the first packet: run the `HardwareDecode` and `ZeroCopy` tests and play an HEVC clip**), `ImageLoader.cpp` (two `#ifdef` lines moved around `convertRgba`), `FileSystem` (`appFilesDir()`), `MainMenuBar.cpp`, `ActionRegistry` (`RebindResult::SystemKey`), `ObjLoader.cpp`, the root `CMakeLists.txt` and `CMakePresets.json` (Mac-only lines), and the tests that now go through `tests/unit/TestMedia.hpp`.
2. **Notarised: done 2026-11-26.** `packaging/out/QUADmapper-1.0.0-macOS-arm64.dmg`, SHA-256 `c65a04262ddcf8970ab4d87d41af5e67b057fd299f64488bce1cde05b4996e75`. The keychain profile is `QM-notary`; a later build is notarised with `packaging/build-mac-app.sh --notarize`. A rebuild gives a new file and a new hash.
3. **On the Mac: the checks nobody but the owner can make.** A projector or second display and Option+P; a clip with sound, by ear; a real MIDI controller; OSC from another machine (macOS will ask about the local network); the notarised disk image opened on a different Mac, ideally one on macOS 13 to 15.
4. **Decide** (ADR-062 section 9): the bundle identifier `com.fiavaion.quadmapper` (cannot change after release); macOS 13 as the oldest; whether the Mac version joins 1.0.0 or a later release (`RELEASE_NOTES.md` has a draft section); whether to draw a 1024-pixel icon.
5. **Then:** merge `feat/macos-port` into `dev` by pull request, and publish the disk image with the release.

- On the Mac: `cmake --preset mac-debug`, build folder `build/mac-debug`, `VCPKG_ROOT` exported. The presets now build the libraries for macOS 13, so a first configure on another Mac rebuilds them (about ten minutes).

## Gate records

- **Full suite (release gate):** `cmake --build build/debug --config Debug`, then `ctest --preset debug | b4dc6f1 (tree 2bf8d00; code as at 494cc40) | 1831 passed, 0 failed, 8 disabled`. 59 of the passes are tests that skip themselves (their test clips are not on this PC). `master` has no commit that `dev` lacks, so the merge gives this same code.
- **`/release`:** PASS on 2026-11-24 at `711cb1b` (1867 passed, 0 failed), see [RELEASE_REVIEW.md](RELEASE_REVIEW.md).
- **Release build and installer** at `828e194`: `dumpbin` shows control-flow guard and CET; a silent per-user install, the harness from the installed copy (procedural, and 1080p H.264 with hardware decoding) and a silent uninstall all passed. The GitHub workflow built the same tree unsigned (run 45629903505, success).
- On this PC use `cmake --preset debug-local`; `ctest --preset debug` runs the tests since `e9885c0`.

## What this session did (2026-11-24 and 25)

- A read-only review of the whole codebase: no critical or high finding, 22 medium, about 55 low. The findings and the fix plan are in `REVIEW/` (ignored by git, on this PC only).
- Batches 0 to 9 of that plan, one fix per commit: privacy, session data, UI, compositions and HAP, media threads, the project loader, other untrusted input, renderer and outputs, build and packaging, core. Three fresh-context reviews (media threads, loader, build and packaging), each fixed in one pass.
- The owed review of `.qmap` 3.5 to 3.7 is done (with batch 5). The file format did not change.
- vcpkg baseline 2026-10-08: FFmpeg 7.1.2 → 9.0.2 (no avfilter), GLFW 3.5.1, GLM 1.0.3, miniaudio 0.11.25. No source change was needed.
- Dead code removed: `ErrorNotification`, `PerformanceMonitor`, `Timer`, `ThumbnailGenerator`, unused `Theme` helpers, 41 uncalled functions.
- Detail: [docs/sessions/2026-11-25-code-review-and-fix-batches.md](../docs/sessions/2026-11-25-code-review-and-fix-batches.md).

## Owner's hands-on test: passed 2026-11-25

The owner tested the application and is happy to move forward. The note does not record which of the items below were tried; the list is kept as the checklist it was.

1. Success metric: add a quad, assign media, Tab and arrows to warp (Shift 10×, Alt 0.1×), F11 to a projector.
2. **Real shows on the new libraries:** play your usual video clips (H.264, HAP) and send them to the projectors. Real-clip tests could not run on this PC. Reverting the update is one commit (`db0b260`).
3. Properties: drag Opacity, a corner value and a crop edge; each is one Ctrl+Z step. Type a value outside a slider's range: it is held to the range.
4. Lock a quad: shape edits are refused with a status line, Edit > Mesh is greyed.
5. Delete a quad, a media item in use, an output and a composition. Each asks first.
6. AI beta switch: off shows no AI menu; on shows AI (Beta) and the engine starts; off again stops it. Then the QM-124 checklist (ADR-053 Amendment 1).
7. Help menu, Ctrl+G, Open Recent, Interface size at 100% and 200%.
8. The 2026-11-23 features:
   - Warp Linear / Perspective on two snapped quads.
   - A projector plugged in while running gets an output and a scene.
   - Grid warp: add a column and a row, Spline / Linear, Warp / Crop.
   - Other scenes On.
   - File > Export Video (the MP4 plays, with sound).
   - A composition with Timeline and Switch, driven by OSC or MIDI.
   - A `.qmap` saved before 2026-11-23 opens unchanged.
9. New this session:
   - "Edit Parameters..." on a pattern in the media bin opens Properties.
   - Start QUADmapper twice: the second brings the first to the front.
   - Editing keys do nothing while Preferences or Export Video has focus.
   - Open and save a show in a folder with a very long path.
   - Does a fresh start show `Untitled*` (modified) in the title? It did once on the three-monitor PC; not investigated.

## Open owner decisions

1. ~~Old personal notes in git history~~ **Decided 2026-11-25: leave history as it is.** The files themselves are untracked since `15af12a`.
2. ~~Procedural patterns need OpenGL 4.3~~ **Decided 2026-11-25 (ADR-062): rewritten as 3.3 fragment shaders**, on `feat/macos-port`. It reaches Windows when that branch merges.
3. **A second `.bak` generation** for shows (ADR-046 describes one as intended).
4. **Engine lock refresh:** `pip-audit` reports 106 advisories in 7 packages of `tools/ai-sidecar/requirements.lock` (Pillow has 76). The engine is not in the installer. It fits with the cu128 move.
5. **Fonts:** eight variants load at start and one is used; JetBrains Mono ships unused.
6. Smaller, from `/release`: Esc on the floating windows; a time limit for an export to a stalled drive; "AI" or "AI (Beta)" in messages and the guide.

## What is left before release

**v1.0.0 is already published** on GitHub (2026-11-24, tag on `5646e8e`, 6 downloads): it is the 2026-11-23 build, before the 94 fixes and the FFmpeg 9 update. Earlier notes called it a draft; that was wrong.

**Owner decisions 2026-11-25:** the tested build replaces 1.0.0 in place (no 1.0.1); stop before publishing; leave git history.

1. Push `dev`, merge PR #3 into `master`.
2. Build the installer from `master` with the "Release installer" workflow (unsigned), and check it with a silent install.
3. **Wait for the owner's "publish".** Then: move the `v1.0.0` tag to `master`, swap the setup exe on the release, and update the SHA-256 in the release text.
4. The installer is unsigned (SignPath declined on 2026-11-24; the workflow signs only from `master` and only when `SIGNPATH_ORGANIZATION_ID` is set). Windows SmartScreen will warn on first run. The policy page can be restored from `7b674c6` if a later application is accepted.
5. After 1.0.0: the AI engine in the installer (TD-44), the sidecar on the PyTorch cu128 builds, decisions 2 to 6.

## Known gaps

- **Not verified:** real-media tests (no clips in `D:\testing Videos` on this PC), `tools/ai-sidecar/setup.ps1` end to end, the runner-built installer installed by hand, the install wizard clicked through, an admin install.
- **Hardening limits:** a control-flow or shadow-stack violation ends the app without the crash report (autosave is the cover). The FFmpeg DLLs are built without these flags.
- **Flaky under parallel load** (both passed in the last three full runs): `AudioSyncTest.RealTimeNullDeviceStaysInStepThroughASeekAndAPause`, `AsyncSourceLifecycle.AReopenedZeroCopyClipTakesOverTheKeptSharedPlanes`. Quarantine either if it fails again.
- **Review items left as they are:** media existence checks run on the main thread at open (an offline share can stall it), OSC matching cost, closed MIDI ports kept in a list, the editor speed items (batch 10, only if the canvas stutters).
- **Logged deferrals:** TD-41..TD-43 and TD-44 in [TECHNICAL_DEBT.md](../TECHNICAL_DEBT.md).
- `D:\testing Videos\Hap\5sec_hap.mov` is a generated test clip from this session; delete it or keep it.

## Hardware-only items

- 🚩 **QM-061**: One-Flash and the AI features on a real projector and surface. Runbook: [docs/QM-061-hardware-test-runbook.md](../docs/QM-061-hardware-test-runbook.md).
- TD-12 present threads, TD-26 audio device latency, TD-37 and TD-39 (projector and camera).

## Reference links

- Session log: [docs/sessions/2026-11-25-code-review-and-fix-batches.md](../docs/sessions/2026-11-25-code-review-and-fix-batches.md)
- Tasks: @TASKS.md (QM-127) · Debt: @TECHNICAL_DEBT.md · Success metric: @CLAUDE.md#success-metric
- Release: [RELEASE_REVIEW.md](RELEASE_REVIEW.md) · `.github/workflows/release.yml` · `packaging/` · [docs/website-launch.md](../docs/website-launch.md)
- Changed this session: `vcpkg.json`, `CMakePresets.json`, root `CMakeLists.txt`, `THIRD-PARTY-NOTICES.md`, `RELEASE_NOTES.md`, `tools/ai-sidecar/README.md`

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
