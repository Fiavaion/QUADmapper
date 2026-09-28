# Session 18 — 2026-06-17 — follow-up fixes + ADR-040 (shared media + crop) scoped & P1 shipped

**Branch:** `dev` (local `f2f253e`, **8 commits ahead of `origin/dev`, unpushed**). `master` =
`fb4c85b` (untouched). **Build:** green. **Tests:** 630/630 ctest (exit 0).

## Narrative

Started from the post-Session-17 clean state (ADR-039 multi-scene complete). The user asked to
hold the `dev`→`master` PR until 🚩 QM-061 (hardware), then to knock out two software-only
follow-ups, then scoped a new feature.

### 1. Groups-on-load fix (`d586da6`)
Quad groups round-tripped into the `.qmap` file but never reappeared in the live scene. Root
causes were **two**, not one:
1. `deserializeQuad` / `deserializeGroup` ignored the persisted `"id"` field, so loaded quads got
   fresh ids while group members still referenced the *saved* ids — the membership linkage was
   already severed at the serializer layer.
2. `syncProjectToEditor` created the quads but never rebuilt the groups.

Fix: restore the persisted id via a defensive `parseIdOrNull` (length-checked + try/catch, so a
malformed/short id falls back to a fresh one — older/hand-edited files still load; this caught a
crash in `BackwardCompatibilityNoTextureBinding` which feeds `"id": "test-uuid"`). Then
`syncProjectToEditor` rebuilds each scene's groups, remapping member + nested-parent references
onto the freshly minted quad/group ids. Test: `V3GroupMembershipRoundTrips`.

### 2. TD-07 — build exit-code flake (`4ca12c5`)
`gtest_discover_tests` raced the default 5s `TEST_DISCOVERY_TIMEOUT` in its POST_BUILD step,
returning non-zero from `cmake --build` while the suite was green. **First attempt** set
`DISCOVERY_MODE PRE_TEST` — reverted, because it broke the project's standard
`ctest --test-dir build/debug` (it then demanded `-C Debug`). **Final:** `DISCOVERY_TIMEOUT 60`,
discovery mode unchanged. Moved TD-07 to resolved in TECHNICAL_DEBT.md.

### 3. ADR-040 scoped & Accepted (`f41a317`)
The user's hypothetical: wrap one panorama video around a box's four faces, two projectors (two
scenes), each face a cropped quarter-strip — without exporting four files in AE/Premiere (which
reintroduces sync drift + 4× cost). An Explore agent mapped the media architecture; the key
findings: `TextureBinding` already has unused `mediaId` + `cropRegion` fields; `SourceManager`
already owns sources + GPU textures but **the renderer never reads it** (the deferred TD-09 gap);
video binds to exactly one quad today (single `videoPlayer`/`videoAssignedQuadId`); output windows
share the GL context (so one texture is sampleable across projectors).

Decision: make `SourceManager` the single media model the renderer reads (via `mediaId`), add a
first-class normalized crop. Then "one decode, many quads" + frame-sync fall out for free. Phased
QM-068–072; `.qmap` additive v3.1. P2 is the risk (re-routing every quad's texture).

### 4. QM-068 — ADR-040 P1 per-quad crop (`f5c2920`, `b3912fc`)
- **Data:** replaced the unused int `cropRegion` with normalized `NormalizedRect cropRect`
  (default full frame) + `isCropIdentity()`/`resetCrop()`.
- **Shader:** `quad.frag` composes crop *outside* the UV transform:
  `uv = u_CropOffset + u_CropSize * uv`. Identity is byte-for-byte the old formula (no-op).
- **Renderer:** sets `u_CropOffset`/`u_CropSize` per quad.
- **UI:** collapsed "Crop (source region)" in the Inspector — X/Width/Y/Height drags + vertical
  quarter quick-sets (1/2/3/4/Full). Collapsed to respect existing clutter.
- **Persistence:** `crop` written inside `textureBinding` only when non-identity (pre-crop files
  + untouched quads stay byte-identical — color/soft-edge precedent; no version bump). Missing
  crop loads as full frame.
- **Verification (user chose GPU readback):** new `test_render_gl` target — hidden GL 3.3 context
  → render a quad textured with a 2×1 red|green source through the real quad shader into an FBO →
  `glReadPixels` the centre. Left-half crop reads red; right-half and right-quarter read green.
  Skip-if-no-context (CI-safe); the repo's first real-GL test. Data round-trip also covered by
  `CropRectRoundTrips` + `MissingCropLoadsAsFullFrame`.

## Gate held
Every change: `cmake --build` green + ctest green (605→…→**630/630**) + success-metric path
untouched (crop identity is a render no-op; all existing render/serialize tests stayed green).

## Next session — resume here
- **ADR-040 P2 (QM-069)** — renderer resolves a quad's texture from `SourceManager` via
  `TextureBinding::mediaId`; dedupe sources by path; retire the `quadTextures` map.
  **Behavior-preserving first**, success-metric-adjacent — start fresh, likely an advisor pass on
  the convergence approach. Then P3 (shared/multi-quad video), P4 (box-wrap proof), P5 (v3.1
  persistence).
- **Push `dev`** to `origin/dev` (8 commits unpushed) when ready.
- **`dev`→`master` PR** still gated on 🚩 QM-061 (hardware) per the user.

## Key files this session
`src/scene/Quad.hpp` (cropRect), `src/shaders/quad.frag`, `src/renderer/Renderer.cpp`,
`src/ui/panels/InspectorPanel.cpp`, `src/project/ProjectSerializer.cpp` (crop + id preserve),
`src/app/Application.cpp` (group rebuild), `tests/unit/renderer/CropRenderTest.cpp` (new),
`tests/unit/project/ProjectSerializerTest.cpp`, `tests/CMakeLists.txt`, `tests/unit/CMakeLists.txt`,
`docs/adr/0040-shared-media-sources-and-per-quad-crop.md` (new).
