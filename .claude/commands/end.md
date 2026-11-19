# /end - End Development Session

End the current QUADmapper session and leave a restart package for the next one.

## What This Command Does

1. **Sync TODOs:** grep `TODO[cl]:` in `src/`, update `notes/TODOs.md` (mark done only with git
   evidence).
2. **Lessons:** append new `LESSON-{ARCH|BUG|PERF|API|UI|TEST|BUILD}-NNN` rows to
   `notes/lessons.md` (never renumber, no duplicates).
3. **Update `TASKS.md`:** statuses, completion dates, the table row for any new QM task, the
   "Last Updated" line.
4. **Rewrite `notes/CURRENT_STATUS.md`** in full (one page): last updated, phase, the last gate
   record (`command | commit | pass/fail counts`), gaps, the next 2–3 tasks, blockers, links.
5. **Write a session log** `docs/sessions/YYYY-MM-DD-<slug>.md`: what happened, decisions and root
   causes, state at end, next steps.
6. **Commit the notes** with `git commit --only <those files>`; never commit product code here.

`SESSION_STATE.json` and `PROJECT_HISTORY.md` are retired (read-only in `docs/archive/`); do not
update them.

## Usage

```
/end
```
