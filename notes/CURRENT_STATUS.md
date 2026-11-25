# Current Status

**Last updated:** 2026-11-25 (code review, fix batches 0 to 9, third `/release` pass, dependency update)
**Phase / focus:** Release 1.0.0. The code work is done; what stands between here and publishing is the owner's hands-on test.
**Dev:** `494cc40`, pushed (`origin/dev` = `494cc40`). 94 commits since the last status note (QM-127).

## Gate records

- **Full suite:** `ctest --preset debug | 828e194 (tree ba8f2c5) | 1827 passed, 0 failed, 8 disabled`. 60 of the passes are tests that skip themselves (their test clips are not on this PC).
- **After it (batch 9):** `cmake --build build/debug --config Debug`, then `test_core` 55, `test_scene` 392, `test_media` 53 `| 494cc40 | 0 failed`. No full suite since `828e194`; the five commits after it touch `src/core`, `src/scene` and `src/media` only.
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

## Owner's hands-on test (about 40 minutes): the release gate

Nothing since 2026-11-20 has been seen in the running app by the owner.

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

1. **Old personal notes in git history** (public repo): rewrite history and force-push, or leave it. The files themselves are untracked since `15af12a`.
2. **Procedural patterns need OpenGL 4.3**, the app asks for 3.3 (review item M22). Today a 3.3-only GPU gets a clear error and the README says so. Rewrite the patterns for 3.3, or raise the requirement with an ADR.
3. **A second `.bak` generation** for shows (ADR-046 describes one as intended).
4. **Engine lock refresh:** `pip-audit` reports 106 advisories in 7 packages of `tools/ai-sidecar/requirements.lock` (Pillow has 76). The engine is not in the installer. It fits with the cu128 move.
5. **Fonts:** eight variants load at start and one is used; JetBrains Mono ships unused.
6. Smaller, from `/release`: Esc on the floating windows; a time limit for an export to a stalled drive; "AI" or "AI (Beta)" in messages and the guide.

## What is left before release

1. The hands-on test above, then its fixes.
2. The `dev`→`master` merge (PR #3 is a draft; [docs/QM-061-pr-prep.md](../docs/QM-061-pr-prep.md)), then one full suite on `master`.
3. Build the installer from `master` with the "Release installer" workflow, and replace the setup exe on the draft v1.0.0 GitHub release: the one attached is the 2026-11-23 build. Then tag and publish.
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
