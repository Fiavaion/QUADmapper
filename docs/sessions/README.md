# Session Logs

Dated development-session logs live here. `/end` writes one per session
(`YYYY-MM-DD-<slug>.md`) with what was done, why, and a **Next steps** section;
`/start` reads the **latest** log here (after `CLAUDE.md`, before
[`../../notes/CURRENT_STATUS.md`](../../notes/CURRENT_STATUS.md)) to reload context.

On conflict, the latest session log is the detailed source of truth; trust it over the
one-page `CURRENT_STATUS.md` summary. Machine-readable state stays in
[`../../SESSION_STATE.json`](../../SESSION_STATE.json); the chronological narrative stays in
[`../../PROJECT_HISTORY.md`](../../PROJECT_HISTORY.md).
