# ADR-045 — "Refined" UI theme redesign

- **Status:** Accepted (2026-06-20, implemented + shown running)
- **Task:** QM-083
- **Source:** Claude Design mockup `QuadMapper Redesign.dc.html` (project `ac1621d4…`, imported via
  the `claude_design` MCP connector). "Variation A — Refined."

## Context

A web (HTML/CSS) redesign mockup of the QuadMapper editor was produced in Claude Design. It is a
**visual refresh, not new features** — it mirrors the existing panel layout (top bar with
Simple/Advanced + Output, left Quad List/Video Player, center stage with corner/edge handles + zoom
toolbar, right Inspector with UV Controls + Geometry, bottom Resources dock + media strip) almost
1:1. QuadMapper's UI is **Dear ImGui** (immediate-mode C++), so the HTML cannot be compiled in; the
faithful translation is the design's **tokens** (palette + typography) applied to the ImGui theme.

The existing theme ("Neon Void", `src/ui/Theme.*`) was already dark + cyan-accented; the mockup is a
*tuning* of it: bluer/lighter panels, a softer cyan, brighter text, and real UI fonts.

## Decision

Re-tint `Theme::Colors` to the Refined palette and bundle the mockup's fonts — **keeping the existing
constant names** so every panel (which all reference `Theme::Colors::*`) re-tints at once, with no
per-panel edits and no layout changes (the layout already matches).

- **Palette** (`src/ui/Theme.hpp`): body `#0a0c10`, panels `#14161b`, inputs `#1f232b`, borders
  `#262a32`, accent cyan **`#45c4ff`** (was pure `#00d2ff`), text `#e6e8ec`, muted `#9aa1ad`, LIVE
  green `#5fd38a`. Values changed; names preserved.
- **Typography**: bundle **Hanken Grotesk** (UI) + **JetBrains Mono** (numeric/paths), both
  OFL-1.1, in `resources/fonts/` (the build already copies `resources/` to the exe dir). The app
  previously shipped no TTFs and ran on the ImGui default font.
- **No layout restructure**: the mockup's structure already maps to the existing panels.

## Consequences

- Whole-app re-skin from one file edit + two font files; fully reversible (git).
- **Not achievable in ImGui** (accepted gaps vs the web mockup): CSS gradients, box-shadows,
  backdrop-blur, the radial-gradient stage background, and exact per-element spacing. The *look*
  (dark, soft-cyan, rounded, two-font) is matched; pixel-parity is not a goal.
- Variable TTFs render at their default (Regular) instance under stb_truetype, so bold/medium weights
  are size-differentiated, not true-weight. A follow-up could bundle static weight files.
- FontAwesome icon font remains absent (pre-existing); `Theme::Icons` glyphs were already unmapped.

## Acceptance

Implemented; the app launches and renders the redesign (verified via window capture
`c:\tmp\qm_redesign.png`): Refined palette + Hanken Grotesk live, layout intact, no crash, success-
metric warp path unaffected.
