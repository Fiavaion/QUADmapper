# TODOs

> **Source of truth for tasks is [TASKS.md](../TASKS.md)** (QM-XXX IDs) and `SESSION_STATE.json`.
> This file only tracks loose code-level `TODO`/`FIXME` comments found by `/todos`, so they
> don't rot in the source. Promote anything real into a QM task in TASKS.md.

## Backlog (from code comments)

- (none: no `TODO[cl]:` tags in `src/` as of 2026-11-19)

## In Progress

- (none)

## Ready for Review

- (none)

## Done (recent, rolling)

- [x] OutputManager "Actual rendering will be done by the Renderer": wired, comment gone in `45efe84` (TD-01, projector output).
- [x] Renderer "Iterate through all quads in scene": multi-quad path in use, comment gone in `669a05c` (ADR-033).
- [x] PreferencesDialog folder browser: Preferences > General > Media library > Browse... opens a folder picker (`AsyncFileDialog::Kind::PickFolder`); checked 2026-11-19.
- Bootstrap audit + augment.
