---
name: mobile-remote-control
description: How to connect a Claude Code session to the Claude phone app for this machine/account
metadata: 
  node_type: memory
  type: reference
  originSessionId: eb4bec4a-5d15-46e9-805c-e5d5cacfada5
---

To control a Claude Code session from the Claude mobile app on this machine, run
`claude --remote-control "<name>"` from a **standalone terminal** (PowerShell/Windows
Terminal), NOT from the VS Code extension chat. The session then appears in the app's
Code tab with a computer icon + green dot; terminal and phone stay in sync.

**Why:** CLI 2.1.176 exposes the feature as the `--remote-control [name]` launch flag, not
the `/mobile` slash command (newer-build only) and not via the VS Code extension runtime
(where `/mobile` reports "isn't available in this environment").

**How to apply:** Same Anthropic account in app + CLI. It starts a *new* interactive
session (flag is set at launch — an existing session can't be retrofitted). Keep that
terminal running; the phone is just a window into the locally-running session.

Note: mobile remote control is NOT GitHub-backed — pushing a repo to GitHub is unrelated
(QUADmapper was pushed to private `Fiavaion/QUADmapper` during this investigation, which is
just a backup, not a requirement for phone control).
