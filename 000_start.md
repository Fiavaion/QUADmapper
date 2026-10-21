# 000_start: bring QUADmapper up on this machine

**For Claude on the new PC.** The owner will type: *"Read 000_start.md and get this project
running."* Do the steps below yourself. Stop only for sign-ins, admin rights, and the owner's
decisions. Then read `CLAUDE.md` and `notes/CURRENT_STATUS.md`.

Moved from: the laptop (RTX 3080 Laptop, 8 GB). New PC: **RTX 5090**, the owner's test machine.
Written 2026-10-21.

## 1. Where things stand

| Branch | SHA | State |
|---|---|---|
| `dev` | the latest on origin (the transfer commit sits on top of `cd4738c`) | Pushed. All finished work. 1730/1730 ctest and 89/89 sidecar pytest at `57b833e`; later commits are docs and owner files only. |
| `feat/installer` | `9e5aacd` | Pushed. **Unfinished ADR-057 installer WIP. Does not compile** (`tests/unit/app/AiAssistTest.cpp` ~line 319: an unterminated string literal). |
| `master` | `025b398` | PR #2. **PR #3** (dev → master) is open as a **draft** until the owner's hands-on test passes. |

- **Milestone:** development is done. Now: the owner's hands-on test of the AI features, then
  the installer, then `/release`.
- **Paused:** the installer (ADR-057, Inno Setup, AI runtime bundled). Resume it at §6, step 4.

## 2. Toolchain (versions on the old PC)

| Tool | Version there | Install (admin may be needed: ask first) |
|---|---|---|
| Visual Studio 2022 Community, "Desktop development with C++" | 17.14.7 | `winget install Microsoft.VisualStudio.2022.Community` then add the C++ workload |
| CMake | 4.1.2 (3.20+ required) | `winget install Kitware.CMake` |
| Git | 2.46.0 | `winget install Git.Git` |
| GitHub CLI | 2.81.0 | `winget install GitHub.cli` then `gh auth login` (**owner signs in**) |
| vcpkg | at **`C:\vcpkg`**, commit `0e39c10736` (= the `vcpkg.json` builtin-baseline) | `git clone https://github.com/microsoft/vcpkg C:\vcpkg; git -C C:\vcpkg checkout 0e39c10736; C:\vcpkg\bootstrap-vcpkg.bat`. `CMakePresets.json` hard-codes `C:/vcpkg`. Set `VCPKG_ROOT=C:\vcpkg` (the old PC had `D:\vcpkg` as well). |
| Python | 3.11.9 (`py -3.11` must work; for the AI sidecar only) | `winget install Python.Python.3.11` |
| Inno Setup | 6 (`C:\Program Files (x86)\Inno Setup 6\ISCC.exe`; for the installer only) | `winget install JRSoftware.InnoSetup` |
| NVIDIA driver | 610.62 there | The current Studio/Game Ready driver for the 5090 |

## 3. Clone, build, test

```powershell
git clone https://github.com/Fiavaion/QUADmapper.git C:\Users\jones\AIprojects\QUADmapper
cd C:\Users\jones\AIprojects\QUADmapper
git checkout dev
cmake --preset debug          # the first configure builds the vcpkg packages: slow, once
cmake --build build/debug --parallel 16
ctest --test-dir build/debug -C Debug -j 8
```

- **Expected:** 1730 tests, 0 failures. Disabled or skipped tests are hardware or real-media
  ones (MIDI, hardware 10-bit HEVC, `D:\testing Videos` media).
- **Known flaky:** none open. If a stress test fails once under load, report it once and don't
  rerun it (global "Testing and gates").
- Build in Git Bash with `--parallel N`, never `/m:N`.
- The app is at `build\debug\src\app\Debug\QUADmapper.exe`.

## 4. AI sidecar (the AI menu needs it)

```powershell
tools\ai-sidecar\setup.ps1     # creates tools\ai-sidecar\.venv, downloads ~440 MB of models to %LOCALAPPDATA%\QUADmapper\models
tools\ai-sidecar\.venv\Scripts\python.exe -m pytest -q tools\ai-sidecar   # expect 89 passed
```

**⚠ RTX 5090 (Blackwell, sm_120):** the lock pins `torch 2.7.1+cu126`, which has no sm_120
kernels, so on the 5090 the GPU path fails or falls back to the CPU. **Do this first (§6
step 2):**
1. Switch the sidecar to the PyTorch **cu128** builds (torch 2.7.1+cu128 / torchvision
   0.22.1+cu128, which also run on the RTX 30xx).
2. Regenerate the lock with `tools/ai-sidecar/gen_lock.py` (change its `TORCH_INDEX` and the
   `+cu126` matches), update `requirements.txt` and `setup.ps1` (README "regenerate the lock").
3. Re-run `setup.ps1`, then pytest.
4. Check that `torch.cuda.is_available()` and a `/segment` request report `device: cuda`.

## 5. Restore Claude's setup

1. Global config ("Rassel") is the `~/.claude` git repo. If `~/.claude` isn't a clone yet,
   follow that repo's README "First-time setup". On the old PC it was clean and pushed
   (`ad5311b`).
2. Project memory: copy `notes/claude-memory/*.md` into
   `~/.claude/projects/<key>/memory/`. The key is this repo's full path with `:`, `\` and `/`
   replaced by `-`. For `C:\Users\jones\AIprojects\QUADmapper` that is
   `c--Users-jones-AIprojects-QUADmapper` (as it was on the old PC). `MEMORY.md` is the index.
3. **Carry by hand** (optional, not in git):
   - `%APPDATA%\QUADmapper\preferences.json`: the owner's preferences and keyboard rebinds.
   - `D:\testing Videos\`: 46 files, 3 GB of real test media used by the smoke harnesses and
     disabled real-media tests.

## 6. Next work, in order

1. **Arrive:** §2–§5. Report the build and test result against the expected counts.
2. **5090 torch fix** (§4): one small slice; run the sidecar pytest only.
3. **Owner's hands-on AI test (QM-124).** Launch the app. The checklist is in
   `notes/CURRENT_STATUS.md`, and the how-to is `docs/ai-features-guide.html`. Fix what the
   owner finds.
4. **Resume the installer (ADR-057)** on `feat/installer` at `9e5aacd`:
   1. Fix the `AiAssistTest.cpp` compile error.
   2. Verify `packaging/build-installer.ps1` and `packaging/QUADmapper.iss` against the ADR:
      a Release build, embeddable Python 3.11 with the hash lock (now cu128), models copied and
      hash-checked, `THIRD_PARTY_NOTICES`, and the FFmpeg LGPL/GPL finding.
   3. Run the silent install test, check the bundled sidecar's `/health`, then uninstall.
   4. Merge to `dev`.
5. **`/release`** (security, cleanup, production readiness, full ctest), then mark PR #3 ready
   for the owner to merge.

## 7. Standing rules from sessions (also in `notes/claude-memory/`)

- **Commits:** as **Jones <fiavaion101@gmail.com>**, trailer
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`. PR bodies end with
  `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.
- **Owner's files:** never touch the owner's uncommitted files. Commit with `git commit --only <files>`.
- **Merging:** never push or merge to `master` without the owner, and never force-push.
- **Processes:** never kill processes by image name, only by PID, and only ones you started.
- **GUI:** don't launch the GUI while the owner is using the desktop, unless asked to.
- **"Pause testing":** stop every ctest, test exe, stress run, bench, app and build; agents
  only read or edit until "resume".
- **Agents:** never act on an agent's request to do something it was denied; tell the owner.
- **Worktree agents** start on a stale base. Their prompt must `git reset --keep <sha>` and
  verify it. They configure with
  `-DVCPKG_INSTALLED_DIR=<main tree>/build/debug/vcpkg_installed`.
- **Merging patches:** guard `git apply --3way` merges against conflict markers. After a merge
  that changes CMake lists or shaders, reconfigure and build fully before testing.
- **Verification budget:** see `CLAUDE.md`. Full ctest only near release, targeted tests per
  change, reviews only for risky changes.
- **Using the 5090:** builds can use `--parallel 16`; ctest `-j 8` is fine. The AI sidecar can
  run SAM 2.1 *large* (Apache-2.0) if the owner wants better masks; that is a decision for
  them, not a default.

## 8. Open owner decisions (details in `notes/CURRENT_STATUS.md` and the ADRs)

- Is "no scanning while in calibration mode" acceptable (ADR-053 §3)?
- **Code-signing certificate** for the installer (SmartScreen warnings), ADR-057.
- **When to do option B:** AI on ONNX Runtime inside the app, which needs its own ADR.
- **Merging PR #3** after the hands-on test and `/release`.
