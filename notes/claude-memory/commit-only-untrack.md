---
name: commit-only-untrack
description: "git commit --only <path> on a path staged with `git rm --cached` commits the disk copy, not the deletion"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 69336402-ac3a-466d-a51f-a3815abe3b02
  modified: 2026-11-19T21:49:59.611Z
---

`git commit --only <path>` takes the path from the working tree. If the path was staged for removal with `git rm --cached` (the file still on disk), `--only` commits the file's current contents as a modification instead of the deletion. This happened 2026-11-19 with `.claude/settings.local.json` and was caught before the push.

**Why:** `--only` re-reads the named paths from disk, so it ignores the staged deletion.

**How to apply:** to commit an untrack, check that the index holds only that change, then run a plain `git commit`. Afterwards check `git show --stat`: an untrack must show deletions only. Related: [[merge-conflict-guard]].
