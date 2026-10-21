---
name: git-identity
description: The git author email to use for commits in the QUADmapper project
metadata: 
  node_type: memory
  type: project
  originSessionId: c198be6b-ae9c-4e42-8b16-fc762b6cc1a8
---

QUADmapper commits use **`Jones <fiavaion101@gmail.com>`** as the git author.

**Why:** The user corrected this on 2026-06-14 — the repo's older baseline commits used
`animaphic@gmail.com`, but the intended project email is `fiavaion101@gmail.com`. No
global git identity is set on this machine, so the repo needs a local one.

**How to apply:** Before committing, ensure `git config user.email` is
`fiavaion101@gmail.com` (set it locally: `git config user.email "fiavaion101@gmail.com"`,
`git config user.name "Jones"`). Note `animaphic@gmail.com` is the user's general account
email ([[#userEmail]] / userEmail), but **commits for this project use fiavaion101**.
