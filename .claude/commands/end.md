# /end - End Development Session

End the current QUADmapper development session with progress saving.

## What This Command Does

1. **Gather Session Data**
   - Collect all tasks completed this session
   - Identify current work in progress
   - Note any decisions made
   - List files modified

2. **Update TASKS.md**
   - Update task statuses
   - Add completion dates to done tasks
   - Update progress percentages for in-progress tasks
   - Recalculate quick status counts

3. **Update PROJECT_HISTORY.md**
   - Create new session entry at top of log
   - Document completed work
   - Record decisions made
   - List files changed
   - Write handoff notes for next session

4. **Update SESSION_STATE.json**
   - Set end time
   - Set status to "completed"
   - Capture current work context
   - Update next steps
   - Save recent files list
   - Update statistics

5. **Generate Session Summary**
   - Show session duration
   - List accomplishments
   - Summarize work in progress
   - Provide handoff notes

## Output Format

```markdown
# Session End - QUADmapper

## Session [N] Summary
**Duration:** [X hours Y minutes]
**Focus:** [Brief description]

## Completed This Session
- [x] QM-XXX: [Task title]
- [x] QM-YYY: [Task title]

## Work in Progress
### QM-ZZZ: [Task title]
**Progress:** [X]%
**Current State:** [Description]
**Next Steps:**
1. [Step 1]
2. [Step 2]

## Files Modified
- `path/to/file.cpp` - [Brief description]
- `path/to/file.hpp` - [Brief description]

## Decisions Made
- [Decision 1]
- [Decision 2]

## Handoff Notes
- [Important context for next session]

---

## Updated Project Status
- **Tasks Completed:** [N]
- **In Progress:** [N]
- **Todo:** [N]

Session state saved. Ready for next session with /start.
```

## Context Limit Handling

If approaching context limits:
1. Prioritize saving essential state
2. Create detailed handoff notes
3. Save all progress to files
4. Suggest user restart session

## Files Modified

- `SESSION_STATE.json` - Updated with session end state
- `TASKS.md` - Updated task statuses
- `PROJECT_HISTORY.md` - New session entry added

## Usage

```
/end
```

or

```
/session-end
```
