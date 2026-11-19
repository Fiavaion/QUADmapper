# Current Status

**Last updated:** 2026-11-19 (owner's release decisions applied)
**Phase / focus:** Release 1.0.0. The audit (QM-125) and the owner's decisions are done and pushed. Next: `/release`, the owner's hands-on test, then the installer (AI engine bundle, TD-44).
**Dev:** `dev` = `origin/dev` (code at `0558a76`; later commits are notes only).
**Gate:** last full suite `ctest --test-dir build/debug -C Debug -j 8 | 4a3a8af | 1766 passed, 0 failed`. Since then, targeted only: `test_editor` 221, `test_ui` 54, `test_app` 28, `layering_check`, `bench_smoke`, `stress_smoke` all pass (uncommitted tree = `0558a76`).

## Decided 2026-11-19 (owner)

- **Licence:** MIT, `LICENSE` (copyright Fiavaion).
- **Version 1.0.0**, set once in `project(VERSION)`; feeds `core/Version.hpp` and the exe's version resource.
- **Release package (TD-44):** icon, VERSIONINFO, Release PDB (`symbols` install component), `cmake --install` gives a flat folder with the vcpkg DLLs, app-local VC++ runtime, resources, notices and licence; FFmpeg written offer. Verified: installed exe started from another folder passed `--stress --hidden`. Left: the AI engine bundle (TD-44, narrowed).
- **OSC** stays on `0.0.0.0` (off until enabled; a tablet controller is the usual use).
- Root `imgui.ini` and `project.qmap` untracked (still on disk); no machine paths in `Bench.hpp`.
- **UI:** Ctrl+G group, File > Open Recent, Preferences > Interface size, Edit > Mesh greyed for a locked quad.
- **AI Assist is a beta**, off by default (`general.aiAssistBeta`, Preferences > General > AI Assist (beta)). Off hides the AI (Beta) menu, the AI Photo panel, Help > AI Features Guide and Calibration's Place From Photo; turning it off stops the engine and gives the show back. The `QM_AI_SELFTEST` hook ignores it.

## Owner's hands-on test (about 25 minutes)

1. Success metric: add a quad, assign media, Tab and arrows to warp, F11 to a projector.
2. Properties: drag Opacity, a corner value and a crop edge; each is one Ctrl+Z step.
3. Lock a quad in the Quad List; corners, mesh keys and Properties shape edits are refused with a status line; Edit > Mesh is greyed.
4. Delete a quad, a media item used by quads, and an output: each asks first, none crashes.
5. With AI Assist (beta) off, there is no AI menu. Tick it in Preferences > General: AI (Beta) appears; AI > Enable AI Assist starts and connects the engine. Untick it: the menu goes and the engine stops. Then, ticked again, the QM-124 checklist (ADR-053 Amendment 1).
6. Help menu: each item opens what it names.
7. New: Ctrl+G with 2+ quads (names "Group N"); Open Recent after a save, a missing file, Clear Recent; Interface size at 100% and 200% (only fonts scale, not padding).
8. Explorer: `QUADmapper.exe` shows the icon and version 1.0.0; the window has the icon.

## What is left before release

1. `/release` (full gate), then fixes from the hands-on test.
2. Installer: `feat/installer` (`9e5aacd`) does not compile and its sidecar lookup conflicts with `dev`; it should use `cmake --install` for the app folder and stage only the AI engine.
3. Sidecar to the PyTorch cu128 builds for the RTX 5090 (`000_start.md` §4).
4. `dev`→`master` PR ([docs/QM-061-pr-prep.md](../docs/QM-061-pr-prep.md)); PR #3 stays a draft until the hands-on test.

Flaky: `AudioSyncTest.RealTimeNullDeviceStaysInStepThroughASeekAndAPause` (20 s real-time) failed once under load on 2026-11-19 and passed alone; flagged, not retried.

## Hardware-only items

- 🚩 **QM-061**: One-Flash and the AI features on a real projector and surface. Runbook: [docs/QM-061-hardware-test-runbook.md](../docs/QM-061-hardware-test-runbook.md).
- TD-12 present threads, TD-26 audio device latency, TD-37 and TD-39 (projector and camera).

## Reference links

- Tasks: @TASKS.md (QM-124, QM-125) · Debt: @TECHNICAL_DEBT.md (TD-41..TD-44) · Success metric: @CLAUDE.md#success-metric
- Decisions: @docs/adr/0035-ai-preshow-sidecar.md (amended) · @docs/adr/0053-ai-surface-proposals-and-structured-light.md

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
