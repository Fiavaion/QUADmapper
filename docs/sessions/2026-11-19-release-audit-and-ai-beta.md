# 2026-11-19: Release audit, owner decisions, AI beta switch

The session ran from 2026-11-14 to 2026-11-19 on `dev`. The lead was Fable 5.2 at high effort, and the session was summarised once along the way.

## What happened

1. **QM-125 release-readiness audit** (`06b17a6`..`4a3a8af`).
   - The work: app start independent of the start folder, a log and crash dump, engine failure paths, undo steps for mesh keys and Properties, the quad lock, panel crash fixes, a token-protected AI engine link, and the user docs.
   - One fresh-context review; its seven findings were fixed in one pass.
   - Full gate `4a3a8af`: 1766 passed.
2. **The owner's release decisions** (`756840f`..`3f2cda4`, pushed to `origin/dev`).
   - MIT licence and version 1.0.0.
   - The release package through `cmake --install` (TD-44, without the AI engine).
   - OSC stays on 0.0.0.0.
   - `imgui.ini` and `project.qmap` untracked; no machine paths in `Bench.hpp`.
   - UI: Ctrl+G, Open Recent, Interface size and the locked mesh menu.
3. **QM-126 AI Assist behind a beta switch** (`31b5e91`, not pushed). The owner asked for it because AI is not part of the MVP and mapping must work without it.
   - The switch is `general.aiAssistBeta`, default off.
   - Off hides every AI entry point.
   - Turning it off calls `AiAssist::disable()`.

## Decisions and root causes

- **`disable()` is separate from `stopSidecar()`.** `stopSidecar()` resets the client, so a switch-off followed by a switch-on could not restart the engine.
- **A stop we asked for resyncs `lastSidecarState_`.** Without that, stopping during Starting posted "AI Assist could not start" (LESSON-ARCH-008).
- **Disabling acts only on the on→off transition,** so the `QM_AI_SELFTEST` smoke hook still runs with the switch off.
- **AI Photo left the Window menu.** It opens from AI > Pick Surface... and Place From Photo, so it cannot be reached while AI is off.

## State at end

- `dev` is ahead of `origin/dev` by `31b5e91` and the notes commit.
- The owner's staged deletion of `.claude/settings.local.json` is still uncommitted, and it stays that way.
- Flaky: `AudioSyncTest.RealTimeNullDeviceStaysInStepThroughASeekAndAPause` failed once under load and passed alone.
- The GUI was not launched for the AI switch.

## Next steps

1. The owner pushes `dev`, or asks for it to be pushed, and types `/release`.
2. The owner's hands-on test (`notes/CURRENT_STATUS.md`, items 1–8); then fixes.
3. Installer and AI engine bundle (TD-44 remainder; `feat/installer` needs reworking onto `cmake --install`).
4. Sidecar to torch cu128; then the `dev`→`master` PR.
