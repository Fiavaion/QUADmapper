---
description: Scan source for TODO/FIXME/HACK comments into notes/TODOs.md
---

Refresh the code-comment TODO list.

1. Grep `src/` and `tests/` for `(TODO|FIXME|HACK|XXX|DEFERRED|TEMPORARY)`.
2. Reconcile with [notes/TODOs.md](../../notes/TODOs.md): add new ones to **Backlog** with a
   `file:line` markdown link and the comment text; remove entries whose comment no longer
   exists in source (it was resolved). Don't paste file contents — just the line + link.
3. Flag any `DEFERRED`/`TEMPORARY`/`TODO Phase` found in a **critical path** (src/scene,
   src/renderer, src/app, src/output, src/video) as a red flag — surface it loudly; per the
   zero-debt rule it should be fixed, not tracked.
4. Remind: the real task list is [TASKS.md](../../TASKS.md) (QM-XXX). Promote anything
   substantive into a QM task there rather than leaving it as a code comment.
5. Report counts: new, resolved, critical-path flags.

Argument (optional): a path to scope the scan.
