---
name: agent-worktree-stale-base
description: "Agent tool isolation \"worktree\" creates QUADmapper worktrees at an old commit (fb4c85b), not dev HEAD — every worktree agent prompt must verify/reset its base first."
metadata:
  node_type: memory
  type: project
  originSessionId: 69336402-ac3a-466d-a51f-a3815abe3b02
  modified: 2026-09-28T18:02:08.976Z
---

On 2026-09-25 every Agent call with `isolation: "worktree"` (5 of 5) started at commit fb4c85b, 71 commits behind `dev`, instead of the current dev HEAD. fb4c85b was the tip of `master` at the time, so worktrees appear to start from master, not dev. Once PR #2 was merged (2026-09-28, master = dev = 025b398) they start current, but they fall behind again as soon as dev moves ahead of master. Each agent only got the right base because its prompt told it to run `git log --oneline -1` and `git reset --hard <expected sha>` before editing.

**Why:** without the check, an agent builds and commits against stale code and the merge back silently reintroduces old behaviour.

**How to apply:** in every worktree-isolated agent prompt for this repo, state the expected base sha and require the reset-if-different step; confirm "did you reset?" in its report. Worktree builds can reuse the main tree's packages with `-DVCPKG_INSTALLED_DIR=C:/Users/jones/AIprojects/QUADmapper/build/debug/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF`. Related: [[build-stale-relink]].
