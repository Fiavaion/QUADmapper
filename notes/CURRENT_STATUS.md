# Current Status

**Last updated:** 2026-11-20 (hands-on round fixes; project moved to the work PC, see [000_SwitchMA.md](../000_SwitchMA.md))
**Phase / focus:** Release 1.0.0. The audit (QM-125), the AI beta switch (QM-126) and two `/release` passes are done. Now: the owner's how-to videos on the work PC, fixing what they turn up; then the installer (AI engine bundle, TD-44).
**Dev:** `origin/dev` = the transfer commit on top of `e74afa3`. Everything is pushed.

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

## What is left before release

1. The owner's hands-on test, then its fixes. `/release` passed twice; its low items are parked in [RELEASE_REVIEW.md](RELEASE_REVIEW.md). Also check:
   - Open and Save with a long folder path give a clear error rather than doing nothing;
   - after Save, the next start shows no recovery prompt.
2. Installer: `feat/installer` (`9e5aacd`) does not compile, and its sidecar lookup conflicts with `dev`. It should use `cmake --install` for the app folder and stage only the AI engine.
3. Move the sidecar to the PyTorch cu128 builds for the RTX 5090 (the lock pins `+cu126`, which has no sm_120 kernels; `tools/ai-sidecar/gen_lock.py` `TORCH_INDEX`).
4. The `dev`→`master` PR ([docs/QM-061-pr-prep.md](../docs/QM-061-pr-prep.md)). PR #3 stays a draft until the hands-on test.

## Known gaps

- **Flaky tests (parallel load):**
  - `AudioSyncTest.RealTimeNullDeviceStaysInStepThroughASeekAndAPause` (20 s, real time): it failed once on 2026-11-19, then passed alone and in both `/release` full runs.
  - `AsyncSourceLifecycle.AReopenedZeroCopyClipTakesOverTheKeptSharedPlanes` (GPU zero-copy): it got no frame within its wait in the 2026-11-20 full run (19.5 s, with other GPU tests beside it), then passed alone (9.5 s).
  - If either fails again, quarantine it or give it a longer wait.
- **Not yet seen in the GUI:** the AI beta switch (item 5) and the 2026-11-19 UI items (item 7).
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
