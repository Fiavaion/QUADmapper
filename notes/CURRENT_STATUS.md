# Current Status

**Last updated:** 2026-11-14 (release-readiness audit, QM-125)
**Phase / focus:** Release preparation. The audit and its fixes are done. Next: the owner's hands-on test, then the installer and release package (TD-44, ADR-057), then `/release`.
**Dev:** the code on local `dev` is at `4a3a8af` (later commits are notes only); `origin/dev` is at `c933d6d`. **Not pushed.**
**Gate:** `ctest --test-dir build/debug -C Debug -j 8 | 4a3a8af | 1766 passed, 0 failed` · `pytest tools/ai-sidecar | 4a3a8af | 119 passed`.

## What the audit changed (QM-125)

- **Start-up:** fonts, layout (`%APPDATA%\QUADmapper\layout.ini`) and logs (`%LOCALAPPDATA%\QUADmapper\logs\`) no longer depend on the start folder. The window opens maximised with the show name in the title. A `.qmap` opens by double-click or drop.
- **Crashes:** a crash writes a minidump to `%LOCALAPPDATA%\QUADmapper\crashes\`, tries an emergency save and tells the user.
- **UI:** status bar with the canvas keys, Help menu (guide, shortcuts, log folder, licences, About), Edit > Arrange and Edit > Mesh, confirmations before deletes, Quad List in front on a first run.
- **Editor:** mesh keys and Properties edits are undo steps. A locked quad keeps its shape against every edit, AI results included.
- **Engine:** OSC, audio, image, texture, HAP and video failure paths hardened.
- **AI engine link:** token header, Origin and Host checks, launch from full paths only, listener checked against our job object.
- **Docs:** user README, `resources/docs/quick-start.html`, full `THIRD-PARTY-NOTICES.md`.

## Owner's hands-on test (about 20 minutes)

Seen working by running the app: start from another folder, fonts, title, status bar, Help menu, saved layout, log file, clean exit. **Not checked by hand:**

1. Success metric: add a quad, assign media, Tab and arrows to warp, F11 to a projector.
2. Properties: drag Opacity, a corner value and a crop edge; each is one Ctrl+Z step.
3. Lock a quad in the Quad List; corners, mesh keys and Properties shape edits are refused with a status line.
4. Delete a quad, a media item used by quads, and an output: each asks first, none crashes.
5. AI > Enable AI Assist: the engine starts and connects (token and listener check are new). Then the QM-124 checklist (ADR-053 Amendment 1).
6. Help menu: each item opens what it names.

## Decisions waiting for the owner

1. Licence: the README and `vcpkg.json` say MIT; there is no `LICENSE` file.
2. Release version number, and one source for it (three places today).
3. Release package (TD-44): install rule, VC++ runtime, PDBs, icon and version resource, FFmpeg source offer. All inside the CMake change boundary.
4. OSC listens on `0.0.0.0` by default: keep, or default to this machine only.
5. Untrack the root `imgui.ini` and `project.qmap`; personal paths in `src/app/Bench.hpp:44-45`.
6. Smaller UI items: Ctrl+G to group, Open Recent, a UI-scale preference, grey out mesh menu items for a locked quad.
7. Push `dev`; PR #3 stays a draft until the hands-on test.

## What is left before release

1. Fixes from the owner's test (QM-124 and the list above).
2. Installer and release package: `feat/installer` (`9e5aacd`) does not compile and its sidecar lookup will conflict with the one now on `dev`.
3. Sidecar to the PyTorch cu128 builds for the RTX 5090 (`000_start.md` §4).
4. `/release`, then the `dev`→`master` PR ([docs/QM-061-pr-prep.md](../docs/QM-061-pr-prep.md)).

## Hardware-only items

- 🚩 **QM-061**: One-Flash and the AI features on a real projector and surface. Runbook: [docs/QM-061-hardware-test-runbook.md](../docs/QM-061-hardware-test-runbook.md).
- TD-12 present threads, TD-26 audio device latency, TD-37 and TD-39 (projector and camera).

## Reference links

- Tasks: @TASKS.md (QM-124, QM-125) · Debt: @TECHNICAL_DEBT.md (TD-41..TD-44) · Success metric: @CLAUDE.md#success-metric
- Decisions: @docs/adr/0035-ai-preshow-sidecar.md (amended) · @docs/adr/0053-ai-surface-proposals-and-structured-light.md

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
