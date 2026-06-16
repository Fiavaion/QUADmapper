# Architecture Decision Records

Every hard-to-reverse decision (library, the `.qmap` schema, a service/threading model,
an architecture pivot) gets a short ADR **written before implementation** — see
[Change Boundaries](../../CLAUDE.md#change-boundaries). Copy [`0000-template.md`](0000-template.md)
to `NNN-<slug>.md`, fill it in, commit it with the change it justifies.

Rules:
- Numbered, append-only. Never edit a decided ADR's history — supersede it with a new one
  and mark the old `Status: Superseded by ADR-XXX`.
- A decision a pending research spike could overturn is `Status: Proposed — pending Spike #N`,
  never `Accepted`.

## Prior decisions (pre-formalization)

ADR-001 through ADR-030 were recorded informally in `SESSION_STATE.json` (`keyDecisions`)
and `PROJECT_HISTORY.md` during the v1.0.0 build, e.g.:

- ADR-027 — Corner pin mode (`CORNERS_ONLY`) is default; mesh edit via checkbox.
- ADR-028 — Default quad size is 40% of canvas, centered.
- ADR-029 — Vertex snapping to other quads with a 2% threshold.
- ADR-030 — Tab cycles corners/vertices; arrows nudge with Shift (10×) / Alt (0.1×) modifiers.

Back-fill any of these into full ADR files here only if a future change needs to reference
or revisit one. New decisions (ADR-031+) start here.
