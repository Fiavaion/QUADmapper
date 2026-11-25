# 2026-11-25: Code review, fix batches 0 to 9, third release pass, dependency update

The session ran on 2026-11-24 and 2026-11-25 on `dev`, on the work PC. The lead was Fable 5.2 at high effort, and the session was summarised several times along the way. It added 94 commits, `5821039`..`494cc40`, all pushed.

## What happened

1. **Code signing.** The SignPath Foundation application was declined on 2026-11-24 (not enough visibility yet). `5821039` and `7b674c6` add the GitHub workflow that builds the installer and signs it when `SIGNPATH_ORGANIZATION_ID` is set; `3d0fe23` removes the code signing policy page and says in the README that the installer is unsigned.
2. **Review (QM-127).** A read-only review of the whole codebase at `3d0fe23`: 0 critical, 0 high, 22 medium, 55 low. It wrote `REVIEW/architecture.md`, `REVIEW/findings.md` and `REVIEW/plan.md`. `REVIEW/` is ignored by git and exists only on this PC.
3. **Fix batches**, one fix per commit:

   | Batch | Commits | What |
   |---|---|---|
   | 0 Privacy | `15af12a` | Local session notes untracked, machine paths scrubbed |
   | 1 Session data | `2291320`, `e9b7ecc` | A failed Open keeps the recovery files; one QUADmapper per session |
   | 2 UI | `15f4d9d`..`fee8aa4` | Group menu, slider ranges, output resolution field, composition delete, mesh drag, editing keys while a dialog has focus |
   | 3 Compositions and HAP | `d1c61ee`..`8e1e6e5`, `56c7c03` | Hap Q colours in a composition, frame size and Snappy length checks, pattern size clamp |
   | 4 Media threads | `48c3508`..`4967724` | Exception guards on decode threads, a read interrupt for files that stop answering, the resampler, finite GPU waits |
   | 5 Project loader | `05f8019`..`25060ee` | 1 GB size cap, finite and range checks, duplicate scene ids, z-order, composition times |
   | 6 Other input | `6912216`..`b42d8f0` | OBJ limits, OSC string length, camera names, engine reply and path checks |
   | 7 Renderer, outputs, app loop | `61df689`..`662d860` | Compute-shader load, DLL search path, texture and shader checks, export guards, engine start and stop, temp files |
   | `/release` | `3d2f1f9`..`711cb1b` | 14 commits, see `notes/RELEASE_REVIEW.md` |
   | Dead code | `786b932`..`8123a30` | Three unused modules, unused `Theme` helpers, 41 uncalled functions; "Edit Parameters..." fixed |
   | 8 Build, packaging, CI | `3461349`..`828e194` | Harness flag ranges, the test preset, long paths, workflow pins, engine locks, vcpkg baseline, Release hardening, installer upgrade |
   | 9 Core | `8c7f642`..`494cc40` | Id generator, log formatting, selection after delete, folder watcher lock, thumbnail cache pruning |

4. **Three fresh-context reviews**, each with its findings fixed in one pass: batch 4 (threading), batch 5 (the loader, together with the owed review of `.qmap` 3.5 to 3.7) and batch 8 (it reaches other machines).

## Decisions and root causes

- **Owner decisions:** a second launch is refused and the first window comes to the front (M12); `ProjectSerializer` hardening approved (batch 5); the build, dependency and workflow changes approved (batch 8).
- **Limits were checked against what the editor can produce.** The review suggested merging grid lines closer than 1e-3 and refusing files over 256 MB. The editor inserts grid lines 1e-4 apart, and about 580 fully warped quads save to more than 256 MB, so the limits became 5e-5 and 1 GB (LESSON-BUG-010).
- **Z-order keeps list order.** Sorting quads by the saved z value would have changed the picture for hand-edited files; loading already drew in list order. The reviewer caught this.
- **`ctest --preset debug` ran no tests.** The test presets named no configuration for the Visual Studio generator. Fixed in `e9885c0` (LESSON-BUILD-009).
- **"Edit Parameters..." did nothing.** It called a hook that no code ever set; found while removing dead code (`76bbcd7`).
- **FFmpeg 9 needed no source change.** The H.264 fixture falls back to software decoding on this PC; an app-exported 1080p clip used D3D11VA with zero-copy, so this is the fixture, not the update.
- **`/sdl` makes C4996 an error,** and about 17 places use `strncpy` or `getenv`, so Release turns that warning off. Debug still shows it.
- **`/PDBALTPATH:%_PDB%` cannot be written in a CMake generator expression** (the percent signs are escaped); the app target uses `$<TARGET_PDB_FILE_NAME:QUADmapper>`.
- **Declined:** installer folder permissions (the suggested Inno directive does not remove inherited write access); MIDI port reuse (a late callback into a reused entry is worse than the small leak); OSC matching cost (unmeasured speed work).

## State at end

- `dev` = `origin/dev` = `494cc40`. The working tree holds only the notes this `/end` writes.
- Last full suite: `ctest --preset debug | 828e194 | 1827 passed, 0 failed, 8 disabled`. Batch 9 after it has targeted tests only.
- GitHub workflow run 45629903505 built the batch 8 tree unsigned (success, 26 minutes).
- The owner has not yet seen anything from this session, or the 2026-11-23 features, in the running app.
- Not verified: real-media tests (no clips on this PC), `setup.ps1` end to end, the runner-built installer installed by hand.
- `D:\testing Videos\Hap\5sec_hap.mov` is a generated test clip left on this PC.

## Next steps

1. The owner's hands-on test (`notes/CURRENT_STATUS.md`), with real video on FFmpeg 9 and projector outputs on GLFW 3.5.1 first.
2. Fixes from it, then the `dev`→`master` merge and one full suite on `master`.
3. The installer built from `master`, the draft v1.0.0 release's setup exe replaced, then tag and publish.
4. The open owner decisions in `notes/CURRENT_STATUS.md` (git history, OpenGL 4.3 patterns, a second `.bak`, the engine lock, fonts).
