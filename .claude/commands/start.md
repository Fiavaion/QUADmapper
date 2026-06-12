# /start - Begin Development Session

Start a QUADmapper development session with automatic context recovery.

## What This Command Does

1. **Load Session State**
   - Read `SESSION_STATE.json` for last session context
   - Read `TASKS.md` for current task status
   - Read `PROJECT_HISTORY.md` for recent session summaries

2. **Display Project Status**
   - Show current phase and progress
   - List tasks: completed, in-progress, todo, blocked
   - Highlight any blockers or issues

3. **Resume Work**
   - If there's work in progress, show context and next steps
   - If no active work, suggest next task based on priority and dependencies
   - Load relevant source files for context

4. **Update Session State**
   - Increment session number
   - Set session status to "active"
   - Record start time

## Output Format

```markdown
# Session Start - QUADmapper

## Project Status
- **Current Phase:** [Phase name]
- **Sessions Completed:** [N]
- **Tasks:** [X] done | [Y] in progress | [Z] todo | [W] blocked

## Last Session Summary
[Brief summary from PROJECT_HISTORY.md]

## Resuming Work
[Current task context and next steps]

---
Ready to continue.
```

## Auto-Continue Behavior

When an incomplete task is detected:
1. Load the task context from SESSION_STATE.json
2. Read relevant source files (from recentFiles)
3. Display the work-in-progress state
4. Propose continuing with the next step

When no incomplete task exists:
1. Find the highest priority TODO task
2. Check that dependencies are satisfied
3. Propose starting that task

## Files Read

- `SESSION_STATE.json` - Machine-readable session state
- `TASKS.md` - Task tracking and status
- `PROJECT_HISTORY.md` - Development history
- `.claude/agent-state/global-progress.json` - Agent coordination state

## Usage

```
/start
```

or

```
/session-start
```
