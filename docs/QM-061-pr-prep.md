# `dev` → `master` PR — prepared body (open after QM-061 passes / review)

**Do not open yet.** The AI wedge's novelty is a hypothesis until 🚩 **QM-061** (real-device
test) passes — see [QM-061-hardware-test-runbook.md](QM-061-hardware-test-runbook.md). The
v1.1 polish (TD-10, US-104, US-101) is independent and could ship separately if desired, but
the plan is one `dev`→`master` merge once QM-061 is green.

**Compare:** https://github.com/Fiavaion/QUADmapper/compare/master...dev
**To open (after approval):** `git push origin dev` then
`gh pr create --base master --head dev --title "<title below>" --body-file docs/QM-061-pr-prep.md`

> Note: as of this writing the v1.1 commits (TD-10, US-104, US-101) are on the **local** `dev`
> only — push `dev` before creating the PR so they're included.

---

## Title

`AI Pre-Show wedge (Phase 9) + v1.1 polish: Surface-Snap, One-Flash, anamorphic, color & soft-edge`

## Body

### What this brings into `master`

**Phase 9 — AI Pre-Show Assist (software-complete; QM-061 hardware gate verified separately):**
- **QM-055** Live camera input source + dependency-free PNG capture (on-quad display → TD-09).
- **QM-056** Optional out-of-process Python AI sidecar (ADR-035): stdlib HTTP server; C++
  `SidecarClient` (WinHTTP + Job Object, kill-on-close, graceful absence). `.qmap`/build/render
  path untouched.
- **QM-057** Surface service — Metric3D metric depth + Open3D plane fit → 4 corners + confidence
  (verticality gate: gallery floor-trap 0.00 vs screen 0.58).
- **QM-058** "Detect Surface" UI — capture → sidecar → apply proposed quad, refine with the
  existing Tab+arrow UX; low confidence → "tap the surface".
- **QM-059** One-Flash ChArUco pattern emit + single-photo capture.
- **QM-060** One-Flash homography solve → push warp (synthetic-validated <0.02, conf 0.99,
  0.13px RMS).
- **QM-062** Audience-perspective ("trap-art") anamorphic warp via the One-Flash homography.

All AI results are **geometry-only** (ordinary quad corners). The Python sidecar is an optional
out-of-build component; `QUADmapper.exe`, the `.qmap` C++ build, and the vcpkg manifest are
untouched by the AI wedge.

**v1.1 polish (independent of AI):**
- **TD-10** Async sidecar start — `SidecarClient::startAsync()` runs attach/launch/poll on a
  worker thread (atomic state + `Starting`); AI-enable no longer freezes the UI ~1–3s.
- **US-104** Per-quad color correction (ADR-036) — brightness/contrast/gamma/RGB gain in
  `quad.frag`; additive `.qmap` field; Inspector section. Identity = render no-op.
- **US-101** Per-quad soft-edge blending (ADR-037) — per-edge feather + gamma in `quad.frag`
  for multi-projector overlap blending; additive `.qmap` field; Inspector section.

### `.qmap` compatibility

Writer version `1.0.0 → 1.1.0` (additive MINOR, ADR-032 §5). New `colorCorrection` and
`softEdge` quad fields are optional and written only when non-identity; old files load with
defaults, and the unchanged major keeps `validateVersion` accepting files both directions.

### ADRs added

ADR-035 (AI sidecar), ADR-036 (color correction), ADR-037 (soft-edge blending).

### Verification

- **Build:** clean (`cmake --build build/debug`).
- **Tests:** 601/601 ctest; 15/15 sidecar pytest (surface tests skip without the GPU model).
- **Success-metric smoke:** green — add quad → Tab-cycle + arrow-nudge warp → F11 projector.
- **US-101** feathering verified visually (temporary env-gated demo screenshot; scaffolding
  removed). **US-104** identity render-no-op verified by smoke + round-trip tests.
- **🚩 QM-061** real-device One-Flash: _attach the documented result + screenshots from the
  runbook here before merging._

### Risk / rollback

- AI wedge is additive and optional (sidecar absent → AI menu disabled; core unaffected).
- v1.1 shader changes are no-ops at identity defaults (existing scenes render byte-identically).
- `.qmap` change is additive and backward+forward compatible (major unchanged).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
