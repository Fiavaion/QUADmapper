# AI Pre-Show Assist — Phased Roadmap ("Snap-to-Surface")

**Date:** 2026-06-16
**Status:** Plan — gated on **ADR-035** acceptance (introduces the AI sidecar dependency).
**Grounding:** [competitor-audit](competitor-audit-2026-06-16.md) ·
[AI prior-art](AI-preshow-mapping-2026-06-16.md) ·
[feasibility spike (GO)](snap-to-surface-spike-2026-06-16.md) · architecture: [ADR-035](../adr/0035-ai-preshow-sidecar.md)

**The bet (validated):** an affordable mapper that **auto-warps a quad onto a real surface from a
phone photo** fills the niche Lightform vacated in 2022 — and the spike proved the capture half on
this exact hardware. Build it as **One-Flash Auto-Warp + human-confirm**, behind an optional
Python sidecar that never touches the 60-FPS path or the build.

**Conventions:** each task carries *checkable* acceptance criteria (the project's "criteria up
front" rule). Verification gate per the project standard: `cmake --build build/debug` →
`ctest` (≥590/590) → smoke harness — **plus**, for any AI-warp task, the result must be *watched*
warping a real surface (green tests ≠ done). Effort: S/M/L. Model tier per `modelswitcher`.

> These QM IDs are **proposed**; they get appended to `TASKS.md` only when ADR-035 is Accepted.

---

## Phase 0 — Enabling foundation (independently useful)

### QM-055 · Live camera input as a media source — M · Sonnet
The capture rail every AI feature needs; also a real table-stakes feature (MadMapper/VPT have it).
- **AC1:** a webcam/phone-cam (and a still-image-from-file path) can be opened and shown as a quad
  source, at ≥15 FPS preview, via the existing source/texture path.
- **AC2:** capture a single frame to a PNG on disk on demand (the input the sidecar consumes).
- **AC3:** no regression — `ctest` ≥590/590, success-metric smoke green.
- **Risk:** low. Touches the media/source path (mind TD-09); no `.qmap` change.

### QM-056 · AI sidecar skeleton + IPC contract (ADR-035) — M · Opus(contract)/Sonnet(impl)
The boundary, with *nothing intelligent behind it yet* — proves graceful presence/absence.
- **AC1:** app can launch/attach to `tools/ai-sidecar/`, health-check it, and tear it down (no
  orphan process).
- **AC2:** versioned `POST /surface` round-trips a stub response; on sidecar-absent/error the AI
  UI disables with a notice and **every existing workflow is unaffected** (smoke green with the
  sidecar both present and killed).
- **AC3:** contract test (app side) + Python unit test (sidecar side) run without a GPU.
- **Risk:** low–med (process lifecycle, loopback binding). Architecture fixed by ADR-035.

---

## Phase 1 — Surface-Snap Assist (lowest-risk AI; projector-off)

### QM-057 · Sidecar surface service (Metric3D → plane → 4 corners) — M · Sonnet
Port the spike from prototype to service; Metric3D (metric depth) fixes the affine bow seen on the
oblique facade.
- **AC1:** `POST /surface{image}` returns the dominant surface's 4 corners + plane + a confidence,
  using Metric3D + Open3D `segment_plane` (ms-scale fit, not the spike's slow Python RANSAC).
- **AC2:** on the 3 spike scenes, the returned corners enclose the intended surface for the
  unambiguous case (screen) and return **low confidence** when the scene is cluttered
  (gallery floor-vs-wall) rather than confidently wrong.
- **AC3:** end-to-end latency < 3s on GPU; documented CPU-fallback number.
- **Risk:** med. Core CV; de-risked by the spike. Pure Python — no app/build impact.

### QM-058 · "Propose corners" UI — human-in-the-loop confirm — M · ui-specialist/Sonnet
The load-bearing finding: AI proposes, human confirms (fully-automatic picking is unreliable).
- **AC1:** capture frame (QM-055) → call sidecar → draw the proposed quad overlaid on the canvas;
  user accepts (writes corner offsets) or nudges with the existing Tab-cycle + arrow-nudge UX.
- **AC2:** low-confidence results prompt "tap the surface" instead of auto-applying.
- **AC3:** success metric intact — the accepted warp is an ordinary quad warp, F11-projectable;
  smoke green. No `.qmap` change (geometry only).
- **Risk:** med. Touches on-canvas editing (success-metric-adjacent) — extract carefully.

---

## Phase 2 — One-Flash Auto-Warp (the wedge)

### QM-059 · Projector test-pattern emit + single-photo capture flow — S · renderer/Sonnet
- **AC1:** the app can flash a known rectangle/ChArUco pattern fullscreen on the chosen output and
  prompt a single phone/camera capture that sees surface **and** pattern together.
- **AC2:** capture round-trips to disk for the sidecar. Smoke green.
- **Risk:** low. Reuses the output/perform path.

### QM-060 · One-Flash homography solve → push warp — M · Sonnet
- **AC1:** `POST /oneflash{surface_img, pattern_img}` detects the projected pattern and the surface
  and returns the homography mapping projector-quad → surface region, as 4 corner offsets.
- **AC2:** applying the result warps the quad so projected content lands aligned on the surface —
  **no projector pose needed** (observed directly), one photo, no gray-code sequence.
- **AC3:** geometry-only result; `.qmap`-safe; smoke green.
- **Risk:** med. Pattern detection robustness is the unknown → QM-061 gates "done".

### QM-061 · 🚩 Real-device validation gate — S · manual/Opus-review
The one risk this environment couldn't close. **Phase 2 is not "done" until this passes.**
- **AC1:** on a *real* projector + *real* surface + a *real* phone photo, One-Flash produces a
  visibly correct warp within tolerance, watched working (per "done means verified").
- **AC2:** documented failure modes (lighting, throw angle, surface texture) + their limits.
- **Risk:** this is the gate, not a build task. If it fails, fall back to porting MIT gray-code
  structured-light (the audit's #B2) behind the same sidecar contract.

---

## Phase 3 — Anamorphic / audience-perspective warp (artist follow-up)

### QM-062 · Audience-viewpoint ("trap-art") warp — M · Sonnet
- **AC1:** from one captured viewpoint, warp content to look geometrically correct from the
  phone's position (the audience sweet-spot) — the anamorphosis use case.
- **AC2:** geometry-only; `.qmap`-safe; smoke green.
- **Risk:** med. Novel, creative; lower priority than the wedge.

---

## Phase 4 — Deferred (separate ADRs when reached)

- **Projector-Off 3D Preview (phone-splat digital twin)** 🔴 — the "wow," big build (GS import + GL
  splat render). New ADR + its own spike before any commitment.
- **Surface-aware generative content** — lowest novelty; add-on once a capture pipeline exists.

---

## Layer A — table-stakes (parallel track; map to existing stories, not renumbered)

These close the credibility gap with free competitors and are *independent* of the AI work. They
already exist as post-MVP stories — listed for sequencing, not re-IDed here:
- **Masking** · **Spout (Win) + NDI** · **Live GLSL source + audio-reactive wiring** ·
  **OSC/MIDI control** (US-102/103 — own dependency ADR) · soft-edge blending (US-101).
- *Highest-leverage shared item:* **QM-055 live camera** serves both a table-stakes source and
  every AI capture flow — do it first regardless of which track leads.

---

## Build order & gates

```
ADR-035 Accepted ─┬─> QM-055 (camera) ─┬─> QM-056 (sidecar) ─> QM-057 ─> QM-058   [Phase 1 ships]
                  │                    └─> QM-059 ─> QM-060 ─> 🚩QM-061 gate       [Phase 2 = wedge]
                  └─ (Layer A table-stakes can proceed in parallel)
                                                      └─> QM-062 (anamorphic)      [Phase 3]
```

- **Start condition:** ADR-035 Accepted (nothing here begins until then).
- **Each task:** ADR/AC first → small build→test→smoke batches → reviewer-mode self-review.
- **Phase 1 is the first shippable increment** (surface-snap assist works projector-off).
- **🚩 QM-061 is the make-or-break** — the wedge's novelty claim is only real once watched working
  on real hardware; until then it stays a hypothesis.
