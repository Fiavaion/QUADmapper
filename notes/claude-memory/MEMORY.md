# QUADmapper — Memory Index

- [Mobile remote control](mobile-remote-control.md) — connect a session to the phone app via `claude --remote-control` from a terminal (not `/mobile`, not the VS Code extension).
- [Keep vs rebuild decision](keep-vs-rebuild-decision.md) — 2026-06-14: refactor-in-place (NOT rebuild); rot is localized to the Application.cpp render() god-function. See ADR-031.
- [Git identity](git-identity.md) — commit as Jones <fiavaion101@gmail.com> for this project (NOT animaphic@gmail.com).
- [Test media location](test-media-location.md) — real images/videos for media verification live in `D:\testing Videos\`; used by the media/image smoke harnesses.
- [AI pre-show initiative](ai-preshow-initiative.md) — "Snap-to-Surface" AI auto-warp wedge (spike GO); ADR-035 accepted; QM-055 (camera+PNG) merged to master, QM-056 (sidecar skeleton, WinHTTP+Job Object) + QM-057 (Metric3D surface service) + QM-058 (Detect Surface UI — Phase 1 ships) + QM-059 (pattern emit/capture) + QM-060 (One-Flash homography solve, synthetic-validated) + QM-062 (anamorphic) done on its branch — ALL software-doable AI tasks complete; the only remaining wedge work is the 🚩 QM-061 real-device test (hardware-only).
- [QUADmapper FFmpeg build](quadmapper-ffmpeg-build.md) — vcpkg ffmpeg HAS libavdevice (webcam via dshow, no new dep) but NO PNG encoder (use the dependency-free PngWriter).
- [Build stale relink](build-stale-relink.md) — `cmake --build --target QUADmapper` sometimes skips the exe relink after a source/generated-header change; `touch` the .cpp (and re-`cmake --preset debug` for shader edits) to force it.
- [Perform stress harness](perform-stress-harness.md) — QM-081 `--stress` headless perform-cycle leak/wedge harness (GlObjectStats + working-set + watchdog + `stress_smoke` ctest); proved the app is leak-clean across perform cycles.
- [HAP playback](hap-playback.md) — QM-080 HAP/HapQ codec playback (ADR-044): FFmpeg-demux + vendored Snappy + capability-gated BCn upload + YCoCg shader; verified end-to-end.
- [Agent worktree stale base](agent-worktree-stale-base.md) — worktree-isolated agents start at stale fb4c85b; prompts must verify + reset to the expected sha; reuse main vcpkg_installed.
- [Merge conflict guard](merge-conflict-guard.md) — guard `apply --3way` + `commit --only` merges: conflicted files otherwise commit with markers.
- [Reconfigure before ctest after a merge](merge-reconfigure-before-ctest.md) — full reconfigure+build before ctest after merging a branch; a lower test count than the agent reported = stale build, not a pass.
- [commit --only and untracking](commit-only-untrack.md) — `commit --only` on a `git rm --cached` path commits the disk file, not the deletion; use a plain commit with only that staged.
- [Verification budget](verification-budget.md) — owner decision 2026-10-02: full ctest only near release, targeted tests per change, reviews only for risky changes, no routine mutation/stress loops/benches; owner hands-on testing first.
