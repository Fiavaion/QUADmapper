# /start - Begin Development Session

Start a QUADmapper development session from the continuity notes.

## What This Command Does

1. **Load context**, in this order:
   - `notes/CURRENT_STATUS.md`: the one-page restart package (phase, gate record, next tasks).
   - The latest log in `docs/sessions/` (by file name): the detailed record; it wins on conflict.
   - `TASKS.md`: task status (QM-XXX IDs).
   - `notes/lessons.md`: the lessons ledger.

2. **Check the gate record** in `CURRENT_STATUS.md` against `git log -1`. If the tree still
   matches, reuse it; do not re-run the suite.

3. **Report and propose**
   - Current phase, what was done last, what is left, blockers.
   - Propose the next task from the status file's list; start only when the owner says so.

`SESSION_STATE.json` and `PROJECT_HISTORY.md` are retired (read-only in `docs/archive/`); do not
read or update them.

## Output Format

```markdown
# Session Start - QUADmapper

## Where things stand
- **Phase:** [from CURRENT_STATUS]
- **Gate:** [record] (holds / stale against HEAD)

## Last session
[Two or three lines from the latest docs/sessions/ log]

## Next
[The next task and its acceptance criteria]
```

## Usage

```
/start
```
