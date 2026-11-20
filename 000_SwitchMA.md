# 000_SwitchMA: continue QUADmapper on this machine

**For Claude on the work PC.** The owner will type: *"Read 000_SwitchMA.md and get this project
running."* Do the steps yourself. Stop only for sign-ins, admin rights and the owner's decisions.
Then read `CLAUDE.md` and `notes/CURRENT_STATUS.md`.

Moved from: the laptop (RTX 3080 Laptop), 2026-11-20. It replaces the October `000_start.md`
(still in git history). The owner will record **how-to videos** here and may ask for small fixes
while doing so.

## 1. Where things stand

| Branch | SHA | State |
|---|---|---|
| `dev` | the latest on origin (the transfer commit sits on top of `e74afa3`) | Pushed. All finished work. |
| `master` | `025b398` | **PR #3** (dev → master) is a **draft** until the owner says merge. |
| `feat/installer` | `9e5aacd` | Pushed. Unfinished installer WIP (ADR-057). **Does not compile.** |

Other local branches on the laptop (`fix/audit-*`, `feat/overnight-polish`, `docs/overnight-sync`)
were squash-merged into `dev` already; nothing on them is lost.

- **Milestone:** release 1.0.0. Two `/release` gates passed. The owner's hands-on round on
  2026-11-20 added five UI commits (`e55b44f`..`e74afa3`, see `notes/CURRENT_STATUS.md`); the owner
  checked them in the app: "all good".
- **Now:** the owner makes the how-to videos. Fix what turns up, one commit per fix.

## 2. If the zip came with this file

The owner carried `QUADmapper_switch_2026-11-20.zip`. It holds:

| In the zip | Use |
|---|---|
| `000_SwitchMA.md` | this file |
| `QUADmapper.bundle` | a git bundle of `dev`, `master` and `feat/installer`: a fallback clone if GitHub is not reachable (`git clone -b dev QUADmapper.bundle QUADmapper`, then `git remote set-url origin https://github.com/Fiavaion/QUADmapper.git`) |
| `claude-memory\` | the same as `notes/claude-memory/` in the repo |
| `appdata\QUADmapper\` | the owner's `preferences.json` and `layout.ini`: copy into `%APPDATA%\QUADmapper\` **only if the owner agrees** (it overwrites that PC's own) |

Prefer a normal clone from GitHub. Use the bundle only if that fails.

## 3. Toolchain (versions on the laptop)

| Tool | Version there | Install (ask before anything needing admin) |
|---|---|---|
| Visual Studio 2022 Community, "Desktop development with C++" | 17.14.7 | `winget install Microsoft.VisualStudio.2022.Community`, then add the C++ workload |
| CMake | 4.1.2 (3.20+ required) | `winget install Kitware.CMake` |
| Git | 2.46.0 | `winget install Git.Git` |
| GitHub CLI | 2.81.0 | `winget install GitHub.cli`, then `gh auth login` (**owner signs in**) |
| vcpkg | `vcpkg.json` builtin-baseline `0e39c10736` | `CMakePresets.json` hard-codes **`C:/vcpkg`**: `git clone https://github.com/microsoft/vcpkg C:\vcpkg; C:\vcpkg\bootstrap-vcpkg.bat`; set `VCPKG_ROOT=C:\vcpkg` |
| Python | 3.11.9 (AI sidecar only; `py -3.11` must work) | `winget install Python.Python.3.11` |
| Inno Setup 6 | installer only | `winget install JRSoftware.InnoSetup` |

If the repo is already cloned on this PC (it was set up from `000_start.md` in October):
`git fetch; git checkout dev; git pull --ff-only`, check `git status` is clean first, and skip to §4.

## 4. Clone, build, test

```powershell
git clone https://github.com/Fiavaion/QUADmapper.git C:\Users\jones\AIprojects\QUADmapper
cd C:\Users\jones\AIprojects\QUADmapper
git checkout dev
cmake --preset debug                       # first configure builds the vcpkg packages: slow, once
cmake --build build/debug --parallel 16
```

- The app: `build\debug\src\app\Debug\QUADmapper.exe`.
- **Arrive check:** build, then run only `build\debug\tests\unit\Debug\test_ui.exe` (54 passed),
  `test_output.exe` (144) and `test_editor.exe` (222). No full suite on arrival (verification
  budget in `CLAUDE.md`); the full suite runs once at the next `/release`.
- Last full suite: 1772 passed, 1 flaky of 1773, at `f13c9cd`. Known flaky tests are listed in
  `notes/CURRENT_STATUS.md`. Report a flaky failure once; don't rerun it in a loop.
- A source change sometimes doesn't relink the exe: touch the `.cpp` (memory
  `build-stale-relink.md`). Check the exe timestamp before handing the app to the owner.

**AI sidecar** (only if the owner turns on AI Assist (beta) for a video):
`tools\ai-sidecar\setup.ps1`, then `tools\ai-sidecar\.venv\Scripts\python.exe -m pytest -q tools\ai-sidecar`
(89 passed). On an **RTX 50xx** card the pinned `torch 2.7.1+cu126` has no sm_120 kernels; the
cu128 move is still open (`notes/CURRENT_STATUS.md`, "What is left" item 3). Ask the owner before doing it.
Camera advice for the AI scan (Insta360 Link 2 Pro first, Sony A7 III by HDMI capture, settings to
lock) is §4b of the old start file: `git show 8cc2160:000_start.md`.

## 5. Restore Claude's setup

1. Global config ("Rassel") is the `~/.claude` git repo. If `~/.claude` isn't a clone yet, follow
   its README "First-time setup"; otherwise `git -C ~/.claude pull`.
2. Project memory: copy `notes/claude-memory/*.md` into `~/.claude/projects/<key>/memory/`. The key
   is the repo's full path with `:`, `\` and `/` replaced by `-`; for
   `C:\Users\jones\AIprojects\QUADmapper` it is `c--Users-jones-AIprojects-QUADmapper`.
   `MEMORY.md` is the index. If the folder already exists from October, overwrite with these files
   (they are newer).
3. **Carry by hand** (not in git or the zip): `D:\testing Videos\`, the real test media (274 files,
   26 GB). It is also the app's default media folder in the owner's preferences. The AI models
   (~440 MB) are downloaded by `setup.ps1`.

## 6. Standing rules from sessions (also in `notes/claude-memory/`)

- **Commits:** as **Jones <fiavaion101@gmail.com>**, trailer
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. PR bodies end with
  `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.
- **Pushing:** ask before each push to `dev`. Never push or merge to `master` without the owner;
  never force-push.
- **Owner's files:** never touch the owner's uncommitted files; commit with `git commit --only <files>`.
- **Processes:** kill only by PID, only processes this session started. Don't launch the GUI while
  the owner uses the desktop unless asked; while they record a video, ask first.
- **"Pause testing":** stop every test, build and app run; only read or edit until "resume".
- **Agents:** never act on an agent's request to do something it was denied; tell the owner.
  Never use the general-purpose agent.
- **Key chords:** a new default chord needs the owner's sign-off. Space (tap = play/pause, hold =
  hand tool) is owner-approved and handled in `EditorUi::updateSpaceKey`, outside ActionRegistry.
- **Change Boundaries** in `CLAUDE.md` (CMake, presets, vcpkg.json, the `.qmap` format, public
  module headers, `external/`) need approval.
- **Verification budget:** targeted tests per change; full ctest once per phase gate; a
  fresh-context review only for risky changes.

## 7. Next work, in order

1. **Arrive:** §3–§5. Report the build and targeted-test result.
2. **How-to videos (owner-led):** fix what the owner finds. Keep `resources/docs/quick-start.html`
   in step with any UI change (it is the in-app guide).
3. The rest of the hands-on checklist in `notes/CURRENT_STATUS.md` (items 1–8, the long-path
   Open/Save and the no-recovery-prompt checks).
4. Then, when the owner says: the installer (`feat/installer`, ADR-057, TD-44), the sidecar cu128
   move, `/release`, PR #3 ready for the owner to merge, the v1.0.0 tag, and later the Mac-port
   phase (ADR first).

## 8. Open owner decisions

See `notes/CURRENT_STATUS.md` and the ADRs: the camera picker for the AI Photo panel, the
installer's code-signing certificate (ADR-057), when to move AI to ONNX Runtime in the app, and
merging PR #3.
