---
name: merge-conflict-guard
description: "When merging agent branches by `git apply --3way` + `commit --only`, guard the commit — a conflicted file gets committed WITH markers otherwise"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 69336402-ac3a-466d-a51f-a3815abe3b02
  modified: 2026-09-27T10:16:15.546Z
---

When integrating worktree-agent branches via `git diff base..branch > p; git apply --3way p; git commit --only <files>`, `commit --only` takes the working-tree content, so a file left with `<<<<<<<` markers is committed silently (happened 2026-09-27: Application.cpp include conflict in the QM-082 merge; caught and amended before push).

**Why:** `--only` bypasses the index's unmerged state; nothing stops the commit.

**How to apply:** chain every merge commit behind a guard:
`[ -z "$(git diff --name-only --diff-filter=U)" ] && ! git grep -nI -e '^<<<<<<< ' -e '^>>>>>>> ' -- src tests docs TECHNICAL_DEBT.md TASKS.md && git commit ...`
Related: [[agent-worktree-stale-base]].
