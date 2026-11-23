# Current Status

**Last updated:** 2026-11-23 (six features on the work PC; website launch kit)
**Phase / focus:** Release 1.0.0. The audit (QM-125), the AI beta switch (QM-126) and two `/release` passes are done. Now: the owner's hands-on test, which must now include the 2026-11-23 features; then the installer (AI engine bundle, TD-44).
**Dev:** `590bedb` plus two docs commits (website kit, this note). `origin/dev` = `590bedb` until those are pushed.

## Features 2026-11-23 (work PC; automated tests only, not yet seen in the GUI)

- `eb4fdae` Per-quad Warp: Linear / Perspective, Linear for new quads, so snapped quads' shared edges line up (ADR-058). `.qmap` 3.5.
- `db1c46c` At startup and on File > New, every connected display gets an output at its own resolution and its own scene; a projector plugged in while running gets one too.
- `d6c22f4` Grid warp panel in Simple and Advanced mode, Spline / Linear points, Image: Warp / Crop, test grid on by default for new quads (ADR-059). `.qmap` 3.6.
- `8e6a30c` Other scenes: On / Off on the canvas, outlines other scenes' quads and snaps to them (ADR-039 amendment).
- `3affcf4` File > Export Video...: one H.264 MP4 per output with AAC sound, rendered offline (ADR-060). `--stress --export-check=<folder>`.
- `590bedb` Compositions: Timeline / Switch cue sources with a GPU crossfade, on any quad, driven by OSC / MIDI (ADR-061). `.qmap` 3.7.
- Gate record (in `eb4fdae`'s message): `test_scene` 383, `test_project` 105, `test_render_gl` 52 (+2 skipped), `test_ui` 54, `test_editor` 209 (+13 skipped, no test media), `test_output` 144, `test_app` 28, `test_ai_assist` 57, `test_renderer` 7, all passed. The other five commits record no counts. No full suite since `f13c9cd`.
- The `.qmap` format went 3.4 → 3.7 (additive; older files load). No fresh-context review is recorded for those format changes; give them one before release.
- [docs/website-launch.md](../docs/website-launch.md): launch kit for the website (copy, facts, the fix-before-launch list).

## Hands-on round 2026-11-20 (owner-requested, owner checked in the app: "all good")

- `e55b44f` The top bar's Play/Pause and Restart drive every video (locked ones through the master clock), ADR-050 Amendment 1. Roomier top bar; nothing shifts when Play becomes Pause.
- `0424a05` Snap: On/Off toggle on the canvas (`general.vertexSnapping`, default on).
- `e939196` Space: a tap plays/pauses, held (0.25 s) it is the hand tool for panning.
- `70d96b7` With one screen, F11 previews the first Automatic output on it (ADR-046 amendment). Outputs tab and Identify (5 s numbers on each projector) in Simple mode (ADR-042 amendment).
- `e74afa3` Scenes tab in Simple mode: one scene per projector, click between them to work on each.
- Gate records (targeted, on the trees that became these commits): `test_editor` 222, `test_ui` 54, `test_app` 28, `test_output` 144, all passed. No full suite since `f13c9cd`.
**Gate:** `/release` **PASS** (2026-11-20); details in [RELEASE_REVIEW.md](RELEASE_REVIEW.md).
- Full suite: `ctest --test-dir build/debug -C Debug -j 8 | 3cace9d..f13c9cd before the verify fixes | 1772 passed, 1 flaky failure of 1773`. The zero-copy test passed alone; see Known gaps.
- After the verify fixes: `test_ai_assist` 57, `test_video_decoder *Seek*` 34 passed, at `f13c9cd`.
- **Owner decisions (2026-11-20):**
  - Network (UNC) paths in a `.qmap` stay allowed, with "open projects only from people you trust" in the quick-start and the release notes.
  - FFmpeg file opens allow only local protocols.
  - `VideoDecoder::seekFrame` is removed.

## Decided 2026-11-19 (owner)

- **Licence:** MIT, `LICENSE` (copyright Fiavaion).
- **Version 1.0.0**, set once in `project(VERSION)`; feeds `core/Version.hpp` and the exe's version resource.
- **Release package (TD-44):**
  - Done: the icon, VERSIONINFO, a Release PDB (`symbols` install component) and the FFmpeg written offer. `cmake --install` gives a flat folder with the vcpkg DLLs, an app-local VC++ runtime, the resources, the notices and the licence.
  - Verified: the installed exe, started from another folder, passed `--stress --hidden`.
  - Left: the AI engine bundle.
- **OSC** stays on `0.0.0.0` (off until enabled; a tablet controller is the usual use).
- Root `imgui.ini` and `project.qmap` are untracked (still on disk); `Bench.hpp` has no machine paths.
- **UI:** Ctrl+G group, File > Open Recent, Preferences > Interface size, Edit > Mesh greyed for a locked quad.
- **AI Assist is a beta**, off by default, also for existing preference files (`general.aiAssistBeta`, Preferences > General > AI Assist (beta)).
  - Off hides the AI (Beta) menu, the AI Photo panel, Help > AI Features Guide and Calibration's Place From Photo.
  - Turning it off stops the engine and gives the projectors back to the show.
  - The `QM_AI_SELFTEST` hook ignores the switch.

## Owner's hands-on test (about 25 minutes)

1. Success metric: add a quad, assign media, Tab and arrows to warp, F11 to a projector.
2. Properties: drag Opacity, a corner value and a crop edge; each is one Ctrl+Z step.
3. Lock a quad in the Quad List. Corner, mesh-key and Properties shape edits are refused with a status line, and Edit > Mesh is greyed.
4. Delete a quad, a media item that quads use, and an output. Each asks first, and none crashes.
5. AI beta switch:
   - With AI Assist (beta) off, there is no AI menu.
   - Tick it in Preferences > General: AI (Beta) appears, and AI > Enable AI Assist starts and connects the engine.
   - Untick it: the menu goes and the engine stops.
   - Tick it again and run the QM-124 checklist (ADR-053 Amendment 1).
6. Help menu: each item opens what it names.
7. New items:
   - Ctrl+G with two or more quads (names "Group N").
   - Open Recent after a save, with a missing file, and Clear Recent.
   - Interface size at 100% and 200% (only fonts scale, not padding).
8. Explorer: `QUADmapper.exe` shows the icon and version 1.0.0; the window has the icon.
9. The 2026-11-23 features:
   - Warp Linear / Perspective on two quads snapped edge to edge: the test grid lines up at the join.
   - Start with a projector connected, and plug one in while running: each gets an output and a scene.
   - Grid warp: add a column and a row, drag a line, Spline / Linear, Warp / Crop.
   - Other scenes On: outlines show and a dragged corner snaps to them.
   - File > Export Video: the MP4 plays in a media player, with sound.
   - A composition on a quad: Timeline and Switch, and Next / Cue n over OSC or MIDI.
   - A `.qmap` saved before today still opens unchanged.

## What is left before release

1. The owner's hands-on test, then its fixes, plus a fresh-context review of the `.qmap` 3.5–3.7 changes. `/release` passed twice; its low items are parked in [RELEASE_REVIEW.md](RELEASE_REVIEW.md). Also check:
   - Open and Save with a long folder path give a clear error rather than doing nothing;
   - after Save, the next start shows no recovery prompt.
2. Installer: `packaging/build-installer.ps1` builds `packaging/out/QUADmapper-Setup-1.0.0.exe` (10.5 MB, Inno Setup 6.7.3, no AI engine). Checked 2026-11-23: silent per-user install, `--stress --hidden` and `--export-check` from the installed copy started in `%TEMP%` (exit 0), silent uninstall left no folder, shortcut or uninstall entry. Not yet: the wizard clicked through by hand, an admin install to Program Files, code signing. Left: the AI engine as a later add-on (TD-44); `feat/installer` (`9e5aacd`) is superseded, `dev` already finds `ai\` beside the exe.
3. Move the sidecar to the PyTorch cu128 builds for the RTX 5090 (the lock pins `+cu126`, which has no sm_120 kernels; `tools/ai-sidecar/gen_lock.py` `TORCH_INDEX`).
4. The `dev`→`master` PR ([docs/QM-061-pr-prep.md](../docs/QM-061-pr-prep.md)). PR #3 stays a draft until the hands-on test.
5. A full-suite gate, since six features landed after the last one.
6. Website launch (section 1 of [docs/website-launch.md](../docs/website-launch.md)): the repo is public since 2026-11-23 and the v1.0.0 GitHub release is a draft with the setup exe attached (publish it after the hands-on test, tagged on `master` after the merge). Left: the exe is not code-signed, and there is no release date. The 32 `worktree-agent-*` branches were deleted on GitHub; they are kept in `D:\MarkTO\AIprojects\QUADmapper-backups\agent-branches-2026-11-23.bundle`.

## Known gaps

- **Flaky tests (parallel load):**
  - `AudioSyncTest.RealTimeNullDeviceStaysInStepThroughASeekAndAPause` (20 s, real time): it failed once on 2026-11-19, then passed alone and in both `/release` full runs.
  - `AsyncSourceLifecycle.AReopenedZeroCopyClipTakesOverTheKeptSharedPlanes` (GPU zero-copy): it got no frame within its wait in the 2026-11-20 full run (19.5 s, with other GPU tests beside it), then passed alone (9.5 s).
  - If either fails again, quarantine it or give it a longer wait.
- **Not yet seen in the GUI:** the AI beta switch (item 5), the 2026-11-19 UI items (item 7) and the 2026-11-23 features (item 9).
- **Logged deferrals:** TD-41..TD-43 (small undo and log-only gaps) and TD-44 (AI engine bundle), in [TECHNICAL_DEBT.md](../TECHNICAL_DEBT.md).

## Hardware-only items

- 🚩 **QM-061**: One-Flash and the AI features on a real projector and surface. Runbook: [docs/QM-061-hardware-test-runbook.md](../docs/QM-061-hardware-test-runbook.md).
- TD-12 present threads, TD-26 audio device latency, TD-37 and TD-39 (projector and camera).

## Reference links

- Session log: [docs/sessions/2026-11-19-release-audit-and-ai-beta.md](../docs/sessions/2026-11-19-release-audit-and-ai-beta.md)
- Tasks: @TASKS.md (QM-124..QM-126) · Debt: @TECHNICAL_DEBT.md (TD-41..TD-44) · Success metric: @CLAUDE.md#success-metric
- Decisions: @docs/adr/0035-ai-preshow-sidecar.md (amended) · @docs/adr/0053-ai-surface-proposals-and-structured-light.md
- User docs touched: `resources/docs/ai-features-guide.html`, `resources/docs/quick-start.html`, `RELEASE_NOTES.md`

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
