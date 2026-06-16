# ADR-032: `.qmap` versioning & migration strategy

**Date:** 2026-06-16
**Status:** Accepted
**Deciders:** Jones

## Context

The `.qmap` project file is a Change Boundary: existing files saved by users must keep
loading. Today the serializer ([ProjectSerializer.cpp](../../src/project/ProjectSerializer.cpp))
has **four read entry points with three different, inconsistent version behaviors**:

| Path | Version behavior today |
|------|------------------------|
| `load()` (`Project`) | rejects anything not `1.x` |
| `deserialize()` (`Project`, from string) | rejects anything not `1.x` |
| `loadToSceneManager()` (the path the app actually uses) | reads `version`, **never validates it** |
| `deserializeScenes()` (from string) | ignores `version` entirely |

Two consequences make this TD-05:

1. **No real migration.** `load`/`deserialize` one-way-reject any non-1.x file
   ([:396](../../src/project/ProjectSerializer.cpp#L396),
   [:478](../../src/project/ProjectSerializer.cpp#L478)). The *first* deliberate format
   bump silently breaks every previously-saved project on those paths, while the scene
   paths half-parse an unknown future file with no error. There is no single, predictable
   rule for "what version do we accept, what do we migrate, what do we refuse."
2. **Copy-paste drift risk.** The quad-into-scene copy block is duplicated **4×**
   (≈ lines 596-608, 633-648, 716-731, 746-760). Every new persisted quad field is four
   edits and four chances to forget one — exactly how a load path silently drops data.

This is hard to reverse because once we ship a format change without a migration contract,
old user files are unrecoverable.

## Decision

Adopt a single, explicit **version policy** and a **migrate-up-on-load** model, and
de-duplicate the load code so the policy lives in exactly one place.

1. **Version model.** Treat `version` as `MAJOR.MINOR.PATCH`. The schema contract is keyed
   on **MAJOR**:
   - `1.x` — legacy single-quad-list format (no `scenes` array).
   - `2.x` — current scene-based format (`scenes` array).
   - The current writer stays at `2.0.0` (no format change shipped by this ADR).

2. **One policy function.** Add a private helper
   `detectAndValidateVersion(json) -> Result<int /*major*/, Error>` used by **all four**
   read paths:
   - missing `version` → error (unchanged).
   - major `1` or `2` → accept, return the major.
   - major `> 2` (a file from a future build) → reject with a clear, actionable error
     (`"Project version X.Y was created by a newer QUADmapper; please update."`) — a
     **predictable forward-incompat refusal**, not a silent half-parse.

3. **Migrate-up on load.** A v1.x file loaded through any path is migrated into the v2.x
   in-memory model (single scene "Scene 1" holding the quads). This already exists ad hoc
   in `loadToSceneManager`; it becomes the one shared routine the legacy paths also use.
   Migration is **in-memory only** — we never rewrite the user's file behind their back;
   the next explicit Save writes v2.x.

4. **De-duplicate.** Extract the repeated bodies into shared statics:
   - `loadQuadInto(Scene&, const json&)` — the quad-copy block (replaces all 4 copies).
   - `loadSceneFrom(json, SceneManager&, bool reuseFirst)` — one scene loader used by both
     `loadToSceneManager` and `deserializeScenes`.

5. **No on-disk format change in this ADR.** Writer output is byte-compatible with today
   (still `version: 2.0.0`, same fields). This ADR only makes *reading* consistent and
   migration-ready. The next time we genuinely add a field we bump MINOR (additive, old
   readers ignore unknown keys) or MAJOR (with a new migration step appended to the chain).

## Consequences

- ✅ Existing `1.x` and `2.x` files load through every entry point with one consistent rule.
- ✅ The first real future format change has a defined home (a migration step + MAJOR bump),
  instead of breaking saved projects.
- ✅ Future-version files fail loudly and helpfully instead of silently corrupting state.
- ✅ One quad-load helper → adding a persisted field is one edit, not four.
- ❌ Cost: ~0.5d and a moderate refactor of `ProjectSerializer.cpp` (a Change Boundary file —
  hence this ADR). Mitigated by: no on-disk change, behavior-preserving for valid files,
  and new migration/round-trip/reject tests added before declaring done.

## Alternatives considered

1. **Rewrite old files to the newest format on load (migrate-on-disk).** Rejected — mutating
   the user's file as a side effect of opening it is surprising and destroys their original;
   migrate-in-memory + write-on-explicit-save is safer and standard.
2. **Leave the legacy `load`/`deserialize` paths as 1.x-only and only fix the scene paths.**
   Rejected — keeps two divergent version rules and the four-way copy-paste; that *is* the
   debt. One policy, one loader.
3. **Introduce a full migration framework (registry of N→N+1 steps) now.** Rejected as
   over-engineering for two majors; a single `1.x→2.x` step plus the forward-reject rule is
   enough, and a registry can be added when a third major actually appears.
