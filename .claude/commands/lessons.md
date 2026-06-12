---
description: Extract LESSON-* tags from source into notes/lessons.md
---

Scan the codebase for inline lesson tags and update the ledger.

1. Grep `src/` and `tests/` for `LESSON-(ARCH|BUG|PERF|API|UI|TEST|BUILD)-\d+`.
2. For each tag not already in [notes/lessons.md](../../notes/lessons.md), add a row:
   ID, tag category, the one-line lesson, and the file:line where it lives.
3. De-dupe by ID. Never delete existing rows.
4. If a lesson implies a durable fix (a rule, a CLAUDE.md line, a skill), note that in the
   "Where it changed the process" column — a lesson that didn't change the process isn't done.
5. Report how many new lessons were added.

Argument (optional): a category to filter on (e.g. `BUG`).
