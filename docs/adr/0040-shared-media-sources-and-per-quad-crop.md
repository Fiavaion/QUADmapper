# ADR-040: Shared media sources + per-quad crop (one video → many quads)

**Date:** 2026-06-17
**Status:** Proposed
**Deciders:** Jones

## Context

A core MVP workflow (user, 2026-06-17): wrap **one** video around a physical box. A cuboid lit
by **two projectors** (so two scenes per [ADR-039](0039-multi-scene-output-routing.md), one per
output) needs the **four side faces** to each show a vertical quarter-strip of the **same**
panorama file, **frame-synced**. The user wants to crop each face's slice in-app — "crop away
the right 75% → that's face 1" — not round-trip through After Effects/Premiere into four exported
files (which also reintroduces the multi-file sync problem and 4× the assets).

Two hard gaps block this today (confirmed by a media-architecture audit):

1. **Video binds to exactly one quad.** There is a single global `EditorState::videoPlayer`, a
   single decoded `videoTextureId`, and a single `videoAssignedQuadId`
   ([Application.cpp:843](../../src/app/Application.cpp#L843)). The same file cannot appear on
   four quads, and four independent players would drift.
2. **There is no crop control.** Cropping is *physically* possible via the existing per-quad UV
   transform — [quad.frag:53](../../src/shaders/quad.frag#L53) computes
   `uv = (texCoord-0.5)·u_UVScale + 0.5 + u_UVOffset` — but only via unintuitive offset/scale
   math, and the UV sliders are the user's pan/zoom, not a crop.

The codebase already points at the intended design. `TextureBinding` carries **two unused,
scaffolded fields** — `UUID mediaId` and `std::optional<Rectangle> cropRegion`
([Quad.hpp:14-29](../../src/scene/Quad.hpp#L14)) — both declared, neither serialized nor read
anywhere. And `SourceManager` ([SourceManager.hpp](../../src/source/SourceManager.hpp)) already
owns media **sources + their GPU textures** with a per-frame `updateAll()` — but **the renderer
never reads it**; it is consulted only by `MediaBinPanel`. That renderer-doesn't-read-SourceManager
gap is exactly the deferred [TD-09](../../TECHNICAL_DEBT.md) "two media models" convergence.

## Decision

Make `SourceManager` the **single media model the renderer reads**, key each quad to a source by
the (currently unused) `mediaId`, and add a first-class **normalized crop rectangle**. The
box-wrap then falls out: N quads referencing one source share one decode + one GPU texture,
sampled N times with N crops — sync is structural, not best-effort.

1. **Per-quad crop rectangle, first-class and normalized.** Replace the unused int-pixel
   `cropRegion` with a **normalized** source sub-rect (`u0,v0,u1,v1 ∈ [0,1]`; default `0,0,1,1`
   = full frame = identity). Applied in the shader as a source-region remap **composed before**
   the existing UV pan/zoom so the two stay independent:
   `final_uv = cropOrigin + cropSize · ((texCoord-0.5)·u_UVScale + 0.5 + u_UVOffset)`.
   At identity crop this is byte-for-byte the current formula (render no-op → existing projects
   look unchanged). A **compact** Inspector control (X / Width, Y / Height, with a "left ¼ / right
   ¼ …" quick-set), mindful of the already-cluttered inspector.

2. **Renderer resolves a quad's texture via `mediaId` → `SourceManager` (the TD-09 convergence).**
   A quad references its media by `TextureBinding::mediaId` into the project-level `SourceManager`,
   which owns the decode and the GPU texture and runs `updateAll()` once per frame in the main GL
   context. The render path looks up `getTexture(mediaId)` instead of the per-quad `quadTextures`
   map and the singular video path. The legacy `quadTextures` / `videoTextureId` /
   `videoAssignedQuadId` / global `videoPlayer` are **retired** (no parallel path).

3. **One decode, many quads, automatic sync.** Because N quads share one `mediaId` → one
   `VideoFileSource` → one decoder → one GPU texture, the cost is **1× decode + 1× upload**
   regardless of quad count, and all consumers see the **same frame on the same clock** — there is
   nothing to drift. The shared texture is sampled by quads in **different scenes / outputs**
   too: output windows share the main GL context
   ([OutputManager.hpp:306](../../src/output/OutputManager.hpp#L306)), so one main-context texture
   is valid in every per-output renderer.

4. **Identical paths dedupe to one source.** Importing/binding a file path returns the **existing**
   source if that path is already loaded. This is the mechanism that makes the box-wrap share one
   decode, and that converges old projects (the same path on four quads) onto one source on load.

5. **`.qmap` additive v3.1 (MINOR), backward-compatible.** Add a project-level `sources` array
   (id, kind, path, playback state) + per-quad `mediaId` + `crop`. Read path: a v3.0-or-older file
   carrying the per-quad `texturePath` is migrated in memory — create-or-dedupe one source per
   distinct path, set each quad's `mediaId`, drop the legacy `texturePath`. No on-disk data loss;
   every existing file still loads (no crop ⇒ identity, no `mediaId` ⇒ migrated from `texturePath`).
   Per [ADR-032](0032-qmap-migration.md)'s single-version policy and the Change-Boundary rule.

## Phased plan (build → test → checkpoint per phase)

- **P1 — Crop rectangle** *(independently shippable, low risk, touches no media model).* Normalized
  `cropRegion` + shader compose + compact Inspector control + `.qmap` v3.1 `crop` field + migration/
  round-trip tests. Works immediately on images and on the single legacy video. **Half the user's
  ask, on its own.**
- **P2 — Renderer reads `SourceManager` via `mediaId`** *(the TD-09 convergence; behavior-preserving).*
  Route image texture resolution through `SourceManager` + `mediaId`; dedupe by path; keep the
  single-video behavior **identical**. Retire the `quadTextures` map. Success metric re-verified.
- **P3 — Shared / multi-quad video.** Retire the singular `videoPlayer` / `videoAssignedQuadId`;
  video becomes a `VideoFileSource` in `SourceManager`; N quads referencing one `mediaId` share one
  decode. Verify 2+ quads play the same file in sync from one decoder.
- **P4 — Cross-scene / cross-output sharing.** The box-wrap proof: one source feeding quads in two
  scenes on two projectors, frame-synced. Verify via the multi-scene smoke + a 2-output run.
- **P5 — Persistence.** `sources` list + `mediaId` + `crop` round-trip in v3.1; old-file migration
  tests; the box-wrap project saved and reloaded intact.

Each phase ends with `cmake --build` + `ctest` green **and** the success-metric warp (Tab/arrow +
F11 projector) intact.

### Acceptance criteria (the gate)
Load one video → place 4 quads → crop each to a vertical quarter (left / mid-left / mid-right /
right) → assign 2 quads to each of 2 scenes on 2 projectors → perform mode: **all four faces show
their slice of the same video, frame-synced, from a single decode**; save + reload preserves
sources, `mediaId`s, and crops. **Verified by running the app**, not tests alone.

## Consequences

- ✅ Resolves TD-09 as a deliberate feature: one media model, renderer reads `SourceManager`.
- ✅ One decode for N quads — sync is structural, cost is flat in quad count.
- ✅ Unblocks **QM-055 on-quad camera display** for free (camera is already a `SourceManager`
  source); procedural sources also become quad-bindable through the same path.
- ✅ Crop is a render no-op at identity ⇒ existing projects are visually unchanged.
- ✅ Reuses the dormant `mediaId` / `cropRegion` fields they were scaffolded for.
- ❌ Touches Change Boundaries: `.qmap` schema (additive v3.1 + migration), the media/render public
  path (`Renderer`, `EditorState`, `ViewportEditor`, `Application` video upload), and removes legacy
  fields — cross-cutting and success-metric-adjacent. Hence this ADR.
- ❌ Largest media change to date; **P2 is the risk** (re-routing every quad's texture lookup).
  Contained by keeping P1 fully independent and making P2 strictly behavior-preserving before P3
  introduces multiplicity — each phase verified on the real app.

## Alternatives considered

- **(a) Pre-render N files (AE/Premiere) + N decoders** — the status-quo workaround. Rejected:
  frame drift across independent decoders, N× decode + asset cost, no live re-crop, painful to
  re-cut.
- **(b) Narrow "shared video texture" hack** — let extra quads sample the one
  `videoAssignedQuadId` texture without converging on `SourceManager`. Faster, but cements a
  *second* parallel media path (more TD-09 debt), doesn't generalize to images/camera/procedural,
  and still can't persist which quad uses what. Rejected against the zero-debt rule.
- **(c) Crop via raw UV offset/scale only (no crop control)** — already possible but the offset
  math is unintuitive and it doesn't address the one-decode requirement. Kept as the underlying
  shader mechanism (P1 composes onto the same UV pipeline) but rejected as the user-facing model.
