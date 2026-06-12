# Technical Debt Tracker

> Zero-tolerance project: this file should normally be near-empty. It exists to make any
> accepted deferral **visible and costed**, not to make debt routine. Core/foundation work
> is never deferred — it compounds 4–7× to fix later. `/start` should halt new feature work
> if any critical item below is open.

## Active debt

| ID | Item | Deferred | Est. cost to fix | Impact | Critical path? |
|----|------|----------|------------------|--------|----------------|
| — | none accepted yet — keep it that way | — | — | — | — |

## To triage in the upcoming audit (not yet classified as debt)

Three `TODO` comments exist in source. The audit should confirm whether each is a real gap
or a stale comment, then either fix or delete:

- [PreferencesDialog.cpp:174](src/ui/dialogs/PreferencesDialog.cpp#L174) — "Open folder browser dialog" (minor UX).
- [OutputManager.cpp:372](src/output/OutputManager.cpp#L372) — "Actual rendering will be done by the Renderer" — **verify this is wired**, not a dead path.
- [Renderer.cpp:324](src/renderer/Renderer.cpp#L324) — "Iterate through all quads in scene and render them" — **verify multi-quad render is actually reached** (could be a real gap given the comment).

## Deferral decision (fill in BEFORE accepting any deferral)

- **Item:** <what>
- **Is it core / user-facing / a foundation others build on?** If yes → **do not defer.**
- **Features that will be built on top:** <0–1 low risk · 2–5 document · 5+ high risk>
- **Cost now vs later:** <Nh now> vs <~Nh × (1 + 0.5 × phases_on_top) later>
- **Justification & mitigation:** <why, and how the risk is contained>

## Resolved debt (evidence required — never "tests pass" alone)

| ID | Resolved | Evidence (fix + verified-working check + no TODO left) |
|----|----------|--------------------------------------------------------|
| | | |
