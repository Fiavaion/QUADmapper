# QUADmapper Tasks

Last Updated: 2026-09-26 (Session 23)

## Quick Status
- In Progress: 0
- Todo: ADR-051 editor architecture / Application.cpp split (with Phase 0 S6: move AI out of
  Application.cpp), TD-12 present threads (hardware), TD-14b, TD-29 running-app check
  (calibration on sliced outputs, done in code), TD-32/33 (pre-existing loop bugs), dev→master
  PR #2, 🚩 QM-061
- Done: Session 22 added QM-083..QM-099; Session 23 added QM-100..QM-109 (PRD MVP gaps + control);
  2026-09-27/28 added QM-117..QM-121 (ADR-047 canvas/slices/blend/crossfades, ADR-049, ADR-056
  zero-copy), TD-30/31 fixed; quiet-machine gates met (see TECHNICAL_DEBT.md)
- Blocked: 0
- Plan of record: [docs/audits/2026-09-25-foundation-audit.md](docs/audits/2026-09-25-foundation-audit.md) §8

## Session 24 (2026-09-27) — ADR-047 scene canvas → output slices

- **QM-120 DONE** — ADR-047 part A: scene canvas in pixels, routing on the output
  (`sceneId`, several outputs per scene), slices (input rect, warp through the ADR-049
  evaluator, L/R/T/B blend zones with gamma and curve, black level), canvas drawn once per
  shown scene and composed per output by the slice shader, takes swap a blended group
  together, `.qmap` 3.4.0 with migration from 3.x (fixture v3.4-canvas-slices.qmap). Pixel
  identity by readback (byte-identical without 3D objects at every rotation; ≤ 2 levels where
  translucent quads lie over a 3D object), blend flatness 0.5% on the GPU. GUI-checked: the
  v3.4 fixture's left projector fades over the overlap on the second monitor; an old keystone
  show projects as in the editor. Quiet-machine bench: quads256 on 2 outputs 170 fps, p99
  10.8 ms, gate met (ADR-047 "Gate e result").
- **QM-121 B1 DONE** — ADR-047 part B1 (editor side, Amendment 3): slice editor in the
  Properties panel for the selected output (list/add/remove/select, input rect, warp corners,
  blend edges, black level; every edit an id-keyed undo step, same-target edits merge); warp
  corners edited on the canvas with the quad keyboard model (Tab/Shift+Tab, arrows, Shift 10×,
  Alt 0.1×) in an explicit slice-warp mode; Auto-blend of facing edges (one undo step); blend
  test pattern (session only, orange banner while on); canvas size in the Scenes panel with
  slice rescale and the canvas lock (`scenes[].canvasLocked`, routing never overwrites a set
  size); calibration restricted to outputs with the default slicing (TD-29; lifted in code on
  2026-09-28 by ADR-047 Amendment 6, running-app check pending). GUI-checked on the v3.4 fixture.
- **QM-121 B2 DONE (2026-09-27, ADR-047 Amendment 4)** — scene crossfades: Preferences fade
  default (0 = cut), OSC `/qm/scene/take i n f seconds`, both canvases mixed in the slice
  shader before blend and black level, blended groups fade together, take-during-fade
  reverses or completes-then-fades, equal-power audio cross-gain ramped by the mixer;
  TD-12 measured (`--bench=present`), vsync holder chosen by refresh rate, mixed-rate
  notice; present threads left in TD-12. GUI-checked on the second monitor (mid-fade and
  end captures). Quiet-machine bench: quads256 on 2 outputs 170 fps, p99 10.8 ms.

## Session 23 (2026-09-26) — PRD MVP gaps, audio, OSC/MIDI (committed on dev, not pushed)

- **QM-100 DONE** — scene take keys 1–9 + numpad (physical keys, any layout; blocked while
  typing/popups/modifiers), from editor and focused output windows; duplicate scene (deep copy,
  fresh ids); reorder (drag, Move up/down); removing an output clears its scene assignments.
  US-008, PRD §4.1.6.
- **QM-101 DONE** — missing media kept on load and relocatable (Missing Media window: Locate
  with same-folder siblings, recursive Search Folder on a worker thread, File > Relocate Missing
  Media); missing surfaces black on outputs, magenta checker in the editor. PRD §4.1.8.
- **QM-102 DONE** — TIF/TIFF via Windows Imaging Component (OS library, no new dependency).
- **QM-103 DONE** — crop edited in source pixels when the size is known.
- **QM-104 DONE** — stills load top row first: every PNG/JPEG/TIF had rendered upside down on
  quads since the baseline (found in GUI QA); OBJ v stored flipped so object textures and video
  on objects are upright; the One-Flash ChArUco pattern is no longer vertically mirrored. Note:
  a saved show that used Flip V to work around it now needs Flip V turned off.
- **QM-105 DONE** — OSC (UDP, off by default) + MIDI with learn (WinMM), control-target registry,
  master blackout (Ctrl+B, Output menu, /qm/blackout), MIDI mappings saved in the show (ADR-048).
  Follow-up: load keeps saved quad ids.
- **QM-106 DONE** — PlaybackClock (media time follows the wall clock, loops wrap without
  restarting), real Ping-Pong (HAP exact; long-GOP via backward GOP decode in a 512 MB cache,
  falls back to Loop with a reason), master timeline + "Lock to master timeline" (ADR-050 §1/3/4).
- **QM-107 DONE** — `--bench` PRD §5 harness + bench_smoke; stress harness moved to
  StressHarness.cpp; per-quad mesh cache (256 quads on one 1080p output 19.7 → ~60 fps Release);
  two-thread file decode.
- **QM-108 DONE** — integration review of QM-100..107 (fresh-context verify): relocation keeps
  timeline lock and MIDI mappings, locked clips refuse OSC/MIDI transport (new /qm/timeline/*
  targets), take refits projectors, queued takes dropped on load, one-Esc rule kept with MIDI
  learn pending, bench skips OSC/MIDI, One-Flash reports blackout.
- **QM-109 DONE** — audio from video files slaved to the clock (hard/soft drift correction,
  ~2 ms max measured error), per-source and master volume/mute, output device, audible only for
  sources in the active or a live scene, OSC/MIDI volume targets (US-012, ADR-050 §2).
- `.qmap` is now 3.3.0 (additive: control, audio, transport timelineLocked/volume/muted, missing
  sources kept); fixture v3.3-control-audio-lock.qmap.
- PRD §5 measured (Release, RTX 3080 Laptop): latency key→swap p95 19 ms uncapped / 34 ms vsync
  (target < 50 ✅); drift < 1 frame/h ✅; no leaks ✅; 256 quads 49–62 fps on one output
  (borderline), 33–45 on two ❌; scene switch p95 21 ms (target 16 ❌, bounded by frame time);
  16× H.264 1080p ❌ (~2.5 fps; needs hardware decode), 16× HAP ~98% of frames ✅-ish.
- GUI QA on the real app: scene keys/duplicate/reorder/rename-guard, missing media dialog +
  Locate + sibling auto-resolve + checker, TIF, pixel crop, orientation fix, Ping-Pong (picture
  runs backwards), timeline lock + master pause/resume, blackout banner — all seen working.
  Audio GUI pass: menu-bar Mute / Vol / device strip, per-source Mute + Volume, "No audio stream"
  and "Silent at non-1x speed" seen; log shows the device opened, mono 44.1 kHz resampled to stereo
  48 kHz, resync on load and on return to 1x, no underruns. Found and fixed: the Video Player did
  not follow the selected quad's video. Sync itself was measured by the audio slice (~2 ms max);
  audible A/V sync, device unplug and 7.1 downmix still need a human ear.

## Session 22 (2026-09-25) — Foundation audit + Phase 0 (committed on dev, not pushed)
Nine-team CEM audit (Opus 5.5 lead) → Phase 0 slices, each gated by a fresh-context Opus-high
verifier. 747/747 ctest (4 audio skips) at 9607a1f.
- **Foundation audit** — [docs/audits/2026-09-25-foundation-audit.md](docs/audits/2026-09-25-foundation-audit.md).
- **QM-083 DONE** — Refined theme (ADR-045) committed.
- **QM-084 DONE** — show-safe persistence (ADR-046): atomic save + .bak, `.qmap` v3.1 `sources`
  table + saved mediaId/transport, fixtures v1.0–v3.1, hardened parsing, deferred file actions,
  unsaved prompt, autosave (off the render thread) + recovery, crash guard. Verify REJECTED the
  first merge (save-before-load overwrote another show) → fixed in d741940 + dd55374.
- **QM-085 DONE** — id-keyed undo (no raw Quad*), undoable full-state delete/duplicate.
- **QM-086 DONE** — perspective-correct corner pin (homography base + exact projective texcoords;
  bezier edges generalised). Was up to 185 px off at 1080p. TD-11 logs the mesh-mode toggle jump.
- **QM-087 DONE (fix pending QM-090)** — projector truth: blend on outputs, GL 3.3 outputs, output
  rotation, focus stays in the editor after F11, projector-paced vsync, monitor identity fields.
- **QM-088 DONE** — playback: frame pacing (30 fps no longer 2× at 60 Hz), unpack alignment,
  HAP playable from UI, stop-flag race, decode backoff + error state, unused-source release,
  ping-pong removed, non-blocking file dialogs.
- **QM-089 DONE** — success metric: canvas owns Tab/arrows while a quad is selected (no extra
  click after "+ Add Quad"), press-only drags, nudge maths extracted + headless-ImGui tests.
- **QM-090 DONE** — monitor identity: desired vs resolved, never auto-fallback onto the
  operator's screen, resolve at F11/hotplug; iconified-output pacing; mirror-proof rotation test.
- **QM-091 DONE** — frame-exact seeks on MPEG-TS (keyframe landing check + backwards retry,
  interruptible so scrubbing never freezes outputs), decoder drained at EOF (no lost B-frames).
- Final verify of the source-lifetime redesign + QM-090 passed; its LOW/MEDIUM follow-ups are TD-14.
- **GUI QA of Phase 0: 10/10 scenarios PASS on the real app** (tests/smoke, BitBlt capture — the
  "dark-video speckles" were a GDI+ CopyFromScreen artefact in the harness, QM-096).

### Phase 3D-A — 3D projection mapping foundation (ADR-052), GUI-verified
- **QM-092 DONE** — ProjectorCamera (OpenCV-convention pinhole, GL matrices proven equal to
  project()), MeshObject, Scene objects, per-output projector + calibration points.
- **QM-093 DONE** — calibrateProjector: DLT/POSIT/homography + LM, robust single-mis-click
  isolation (>= 9 pts), warnings; Auto solves the lens only from >= 8 points and only keeps a
  plausible lens (QM-099).
- **QM-094 DONE** — OBJ import, `.qmap` 3.2.0 (objects + projector), object list/Inspector.
- **QM-095 DONE** — objects rendered from each output's projector (depth, previews, editor).
- **QM-097 DONE** — calibration UI: model view pick (vertex snap), canvas placement, Tab/arrow
  nudge, live solve, per-point error, warnings.
- **QM-098 DONE** — every projector fits its output's current size; uncalibrated ones re-frame
  on their objects each frame.
- **QM-099 DONE** — object textures upright (reverted a double flip), stable Auto lens, New
  resets calibration. GUI QA: import, rotation, no-output scene, 6-point calibration + nudges,
  save/load (within 1-2 px), object safety, success metric — all PASS.
- **Next (needs approval: new sidecar dependencies/ADR-053)** — 3D-B camera-assisted
  calibration (Gray-code structured light + ChArUco camera calibration → automatic point pairs),
  3D-C AI proposals (SAM 2 masks, Depth Anything V2 surfaces), 🚩 QM-061 real camera test.

## Session 21 (2026-06-19) — MultiMon cross-pollination (built + verified, uncommitted)
Cross-pollinated from the MultiMon project (`D:\AIprojects\MultiMon`). Model-switched build
(Opus design/verify + spot-checks, Sonnet impl). Harness-first then HAP. **654/654 ctest, clean build.**
- **QM-081 DONE (H0–H3, + H4 hap subset)** — perform-mode stress/leak harness. Headless `--stress`
  EnterPerform/ExitPerform cycle runner: `core/GlObjectStats` GL-object live-count gate + working-set
  backstop + wedge watchdog + `--hidden` + `stress_smoke` ctest. Proved the app leak-clean across
  perform cycles. **H4 hap subset DONE** — `--source=hap`/`--media=PATH` + `--pause-after` pause/resume
  recovery assert; stressed the HAP path 10 cycles: GL flat, WS delta 0 MB, pause/resume recovers
  every cycle (no LESSON-BUG-006 freeze). Spec: `docs/perform-stress-harness-spec.md`.
  **H4 TODO (remainder)** — PNG-capture visual gate.
- **QM-080 DONE + VERIFIED** — HAP / HapQ codec playback (ADR-044). FFmpeg-demux (no FFmpeg HAP
  decoder) + vendored decode-only Snappy + `hap::parseHapFrame` (16 tests, bounds-validated) +
  capability-gated BCn upload (`glCompressedTex*`, s3tc/bptc gate) + HapQ YCoCg→RGB shader.
  End-to-end verified: real `D:\testing Videos\Hap\5sec_hap.mov` (1920×1080 Hap Q) renders
  correct-colored through the real pipeline (capture: `c:\tmp\a5_hapq_shader.png` vs `_raw.png`).
  ADR: `docs/adr/0044-hap-codec-playback.md`. `SourceType` unchanged; `.qmap` unaffected.
- **QM-082 DONE** — Convert to HAP / HAP Q from the media bin: own encoder (linked FFmpeg has no libsnappy, so no HAP encoder) with vendored stb_dxt + Snappy, sound copied or re-encoded to AAC, progress/cancel window, undoable Replace in show.

## Session 20 (2026-06-19) — Mapping-editor UX overhaul (committed + pushed)
All built + 633/633 ctest; committed + pushed to `origin/dev` (`721b61a` code + `83deff2` docs).
- **QM-073 DONE** — viewport zoom/pan + aspect-correct canvas (ADR-041): central-node render,
  `u_ViewScale`/`u_ViewOffset` shaders, Fit/wheel/middle-drag, portrait auto, hit-test inverts transform.
- **QM-074 DONE** — native "Load Video…" dialog + crop reworked to Left/Right/Top/Bottom % insets.
- **QM-075 DONE** — Simple/Advanced UI modes (ADR-042): persisted toolbar toggle; curated Simple
  panels; video auto-play on assign; canvas drag-drop (manual release); click-to-select quad;
  centralized `assignMediaToQuad` (fixed video-as-image bug); Scenes "New" seeds a quad.
- **QM-076 DONE** — media-bin video first-frame thumbnails + procedural thumbnails + hover-to-play.
- **QM-077 DONE** — Undo/Redo (Ctrl+Z/Y + Edit menu) for corner/mesh drag+nudge, rename, grid
  edits; crash-safe via history-clear on delete/scene-switch. (Create/delete undo deferred.)
- **QM-078 DONE** — Report Bug button; smooth 32×32 corner-pin tessellation (no fold); bezier
  edges on the 4 corners (ADR-043): Coons patch, amber handles, toggle+reset, .qmap, undo.
- **QM-079 DONE (2026-09-27, ADR-049)** — bezier handles on grid division lines: every control
  vertex has optional tangent handles along its row and column (two per axis), on the unified
  surface evaluator S = B + R. Mesh edit: `H` / `Shift+H` cycles the selected vertex's handles,
  arrows nudge them (Shift 10×, Alt 0.1×), mouse drag works, undoable; saved as `meshTangents`.

## Session 18 (2026-06-17) — ADR-040 scoping (shared media sources + per-quad crop)

**Proposed: [ADR-040](docs/adr/0040-shared-media-sources-and-per-quad-crop.md).** Lets ONE
decoded video file feed MULTIPLE quads, each cropped to a sub-region, from a SINGLE decode —
so a panorama can wrap a box's faces across 2 projectors, frame-synced, with no AE/Premiere
pre-cut. Converges TD-09 (renderer reads `SourceManager` via `TextureBinding::mediaId`) and
wires the dormant `mediaId`/`cropRegion` fields. `.qmap` → additive **v3.1** (sources list +
per-quad `mediaId` + `crop`; old files migrate). Phased:

- **QM-068 (P1) DONE** — per-quad normalized crop rectangle. `TextureBinding::cropRect`
  (`NormalizedRect`, default full frame); `quad.frag` composes crop outside the UV transform
  (`uv = cropOffset + cropSize·uv`; identity = byte-for-byte no-op); Renderer sets
  `u_CropOffset`/`u_CropSize`; collapsed Inspector "Crop (source region)" control with
  X/Width/Y/Height + vertical-quarter quick-sets (1/2/3/4/Full); `.qmap` writes `crop` inside
  `textureBinding` only when non-identity (pre-crop files byte-stable; missing crop → full
  frame). **Verified:** `CropRectRoundTrips` + `MissingCropLoadsAsFullFrame` (data) **and a
  real-GL `test_render_gl` readback** (left-half crop renders red, right-half/right-quarter
  render green through the actual shader → FBO → glReadPixels). 630/630 ctest. Commits
  `f5c2920` + `b3912fc`.
- **QM-069 (P2) DONE** — renderer resolves a quad's texture via `mediaId` → `SourceManager`
  through the shared `buildQuadTextureMap()` (editor/preview/output); `SourceManager::updateAll()`
  runs per frame. Image Load/drag-drop/`.qmap`-load create-or-dedupe an image source by path
  (`getOrCreateImageSource`) and set the quad's `mediaId`. UV grid + legacy single video stay on
  the `quadTextures`/wrapper fallback (full retirement → P3 + a grid-source follow-up). Surfaced
  + fixed two dormant latent bugs: every `TextureSource::id_` was default-null (so `registerSource`
  rejected it) and `Texture::update` never regenerated mipmaps (minified CPU-source textures
  rendered black). **Verified on screen** (`drive_image_smoke.ps1`: a real PNG binds via `mediaId`
  and renders on the warped quad). 630/630 ctest.
- **QM-070 (P3) DONE** — shared / multi-quad video. Retired the singular `videoPlayer`/
  `videoTextureId`/`videoAssignedQuadId` + the manual per-frame upload loop; video is a
  `VideoFileSource` in `SourceManager`, updated by `updateAll()`. The Video Player panel drives
  one active source and assigns it to a quad by `mediaId` — the **same video on N quads decodes
  once**. Fixed a migration-surfaced crash (`createGPUTexture` provisioned RGBA but video frames
  are RGB(3); `uploadFrameToTexture` now reallocs on a channel mismatch). **Verified:** new
  deterministic GL test (`test_source_gl`) loads/plays/advances/uploads; GUI confirms panel +
  load + per-quad assign.
- **QM-071 (P4) DONE (software)** — cross-scene / cross-output sharing is emergent from P2's
  global `buildQuadTextureMap` (iterates every scene) + P3's project-global source: one video
  assigned to quads in 2 scenes resolves for both outputs (textures shared across the shared GL
  context). No new code. The on-projector frame-synced proof is the 🚩 hardware test.
- **QM-072 (P5) DONE** — persistence. Video assignment records the quad's path in the same
  `texturePaths` carrier images use, so it serializes with the project (no schema change — reuses
  P2's path + P1's crop). On load, `syncProjectToEditor` routes each path by media type
  (`getOrCreateVideoSource` deduped / `getOrCreateImageSource`) and rebinds by `mediaId`; the
  box-wrap reloads as one shared decode. Video/image dedupe-by-path tests; serializer already
  round-trips `texturePaths`.

Gate per phase: `cmake --build` + `ctest` green + success-metric warp intact.

## Session 18 (2026-06-17) — follow-up fixes (dev)

- **Groups-on-load FIXED** (`d586da6`): `deserializeQuad`/`deserializeGroup` preserve the
  persisted id (defensive parse); `syncProjectToEditor` re-applies each scene's groups with
  member + nested-parent remap. Test `V3GroupMembershipRoundTrips`.
- **TD-07 RESOLVED** (`4ca12c5`): `gtest_discover_tests DISCOVERY_TIMEOUT 60` ends the spurious
  `cmake --build` non-zero exit. 625/625 ctest.

## Session 16 (2026-06-17) — mapping-editor fixes + multi-scene output routing (ADR-039)

**Mapping-editor UX fixes (committed):** on-canvas handle picking normalized by window
size (DPI-correct); round aspect-corrected handles; mesh-grid wireframe in edit mode;
clean 1×1 default grid (`MIN_MESH_RESOLUTION=1`); **drag-to-position** division insert
(arm → press → drag → release commits + auto-disarms, Esc cancels); prominent
"Reset Transform" button. 618/618 ctest.

**Multi-scene + per-output routing — [ADR-039](docs/adr/0039-multi-scene-output-routing.md) (in progress).**
Revives the multi-scene track retired in ADR-033 (was unwired dead code; revival was
anticipated). A project holds multiple scenes, each a separate mapping assigned to **one**
output; each output projects its assigned scene; default scene auto-assigns to the 2nd
monitor; live per-output preview. `.qmap` → additive **v3** (still reads v1/v2). Phased:

- **QM-063 (P1) DONE** — multi-scene runtime foundation: `EditorState` holds a scene list +
  active-scene accessor (`activeScene()`/`quadManager()`), lazily guarantees one scene.
  Behaviour-preserving (one scene today). Committed `af708c3`. 618/618 ctest.
- **QM-064 (P2) DONE** — output routing: `Scene::assignedOutputId`; `renderToOutputs` renders
  each output's assigned scene; pure unit-tested `sceneForOutput()`; default scene → 2nd monitor
  at startup. **Verified on real 2-monitor hardware** (perform mode: each projector showed its
  own scene's quad). Committed `6c712a4`. 619/619 ctest.
- **QM-065 (P3) DONE** — Scenes UI: a "Scenes" tab in the Resources panel (`ScenePanel`)
  with New/Delete/rename, active-scene switch, and a per-scene output-assignment dropdown
  (one-scene-per-output enforced). Viewport/QuadList/Inspector follow the active scene via
  the existing `quadManager()`/`activeScene()` accessors. **App-verified** (synthetic-input
  smoke `tests/smoke/drive_scenes_smoke.ps1`: created Scene 2 → auto-active, Delete enabled
  at 2 scenes, default scene shows "Projector 1", new scene "(no output)"). 619/619 ctest.
- **QM-066 (P4) DONE** — live per-output preview: new RAII `Framebuffer` (renderer);
  `OutputManager::updatePreviews()` renders each output's assigned scene to a 320×180
  offscreen FBO in the main GL context (saves/restores framebuffer+viewport); a "Preview"
  tab (`OutputPreviewPanel`) shows each output's thumbnail (V-flipped) + "name → scene".
  **App-verified** (Preview tab showed "Projector 1 → New Scene" with the default quad
  rendered on black). The reference `PreviewPanel` from `669a05c^` was an interactive vector
  editor (overlaps ViewportEditor), so P4 ships a true FBO thumbnail instead. 619/619 ctest.
- **QM-067 (P5) DONE** — `.qmap` v3 persistence. Serializer writes v3 (scenes[] + outputs[])
  when the project has scene DTOs, else legacy v1.2 flat; unified reader yields >=1 scene
  for v1 (flat→1 scene) / v2 (scenes→list) / v3 (+outputs); `kMaxSupportedMajor` 2→3.
  `Project` gained `SceneData`/`OutputData` DTOs. Application `syncEditorToProject`/
  `syncProjectToEditor` bridge ALL scenes + outputs (load rebuilds outputs and remaps each
  scene's `assignedOutputId` onto the live output ids; falls back to assigning the active
  scene so v1/v2 files still project). **App-verified**: saved default → v3 file with
  output+scene+assignment; hand-crafted 2-scene v3 loaded → Scenes tab "Alpha→Beamer" +
  "Beta→(no output)", Preview "Beamer → Alpha" rendering the quad. 5 migration unit tests;
  624/624 ctest.

Gate per phase: `cmake --build` + `ctest` green + success-metric warp intact.

---

## Session 15 (2026-06-16) — v1.1 polish + QM-061 prep
- **TD-10 RESOLVED** — async sidecar start (`startAsync()` worker thread); AI-enable no longer freezes the UI. See [TECHNICAL_DEBT.md](TECHNICAL_DEBT.md).
- **US-104 DONE** — per-quad color correction (brightness/contrast/gamma/RGB), [ADR-036](docs/adr/0036-per-quad-color-correction.md). Shader + Inspector + additive `.qmap` field. Identity = render no-op.
- **US-101 DONE** — per-quad soft-edge blending (per-edge feather + gamma), [ADR-037](docs/adr/0037-per-quad-soft-edge-blending.md). Verified visually (feathered projector screenshot). Additive `.qmap` field.
- **QM-061 prepped** — turnkey hardware runbook [docs/QM-061-hardware-test-runbook.md](docs/QM-061-hardware-test-runbook.md) + ready `dev`→`master` PR body [docs/QM-061-pr-prep.md](docs/QM-061-pr-prep.md).
- **UV test-grid (#2) DONE** — Blender-style colored UV grid texture + "UV grid on new quads" toggle + Inspector "Apply UV Test Grid". Verified visually warped on quad+projector. `src/renderer/UvTestGrid.*`, `test_renderer` (4 tests).
- **Kantan grid (#3) — ADR-038 ACCEPTED; Tier 1 COMPLETE.** [docs/adr/0038-kantan-style-grid-warp.md](docs/adr/0038-kantan-style-grid-warp.md). Done: batch 1 (non-uniform control-grid model, behavior-preserving), batch 2 (serializer + migration, `.qmap` 1.1→1.2), batch 3a (Inspector add/remove divisions), batch 3b (viewport click-on-edge insert via inverse-bilinear). Non-uniform grid warp fully usable (Kantan **Linear** parity). Next: Tier 2 (Bezier handles + tessellation), field-test, Tier 3 (local subdivision). `.qmap` writer **1.2.0**.
- **3D scan / model-UV (#4) — SHELVED (2026-06-17).** Deliberate decision to keep focus on the 2D wedge; parked (not rejected). Roadmap/spike preserved at [docs/research/3d-scan-uv-projection-roadmap-2026-06-16.md](docs/research/3d-scan-uv-projection-roadmap-2026-06-16.md).
- Gate held all session: build clean, 605/605 ctest, success-metric smoke green. `.qmap` writer bumped 1.0.0 → 1.1.0 (additive MINOR, ADR-032 §5).

---

## Phase 9: AI Pre-Show Assist ("Snap-to-Surface")

ADR-035 **Accepted** (optional out-of-process Python sidecar; render path / build / `.qmap`
untouched). Plan: [docs/research/ai-preshow-roadmap-2026-06-16.md](docs/research/ai-preshow-roadmap-2026-06-16.md).
Grounded by the prior-art research, competitor audit, and a GO feasibility spike (all in
`docs/research/`). Each task: checkable AC first → small build→test→smoke batches. The wedge's
novelty is a hypothesis until **🚩 QM-061** (real-device test) passes.

#### QM-055: Live camera input as a media source (Phase 0 — enabling)
- **Status:** DONE (capture rail + PNG) — **Completed:** 2026-06-16. On-quad *display* deferred to TD-09 (user decision).
- **Phase:** Phase 9 · **Effort:** M · **Depends On:** none (capture rail for all AI features)
- **Delivered & verified on the real "HD User Facing" webcam (1280×720/30fps mjpeg):**
  - `VideoDecoder::openCamera()` — FFmpeg dshow capture; `libavdevice` linked (already in vcpkg, no new dep; CMake one-liner approved). `open()` refactored into shared `setupDecoderFromFormat()` (behavior-preserving; media smoke confirms video→quad→projector intact).
  - `CameraSource : TextureSource` (live, non-playable; drains to newest frame) + `CameraDevices` DirectShow enumeration + `SourceManager::createCameraSource`. Test group `test_source` (2 tests, real-hardware, skip-if-absent).
  - **AC2 (sidecar input):** `writeRgbPng()` — dependency-free PNG encoder (default vcpkg ffmpeg has no PNG encoder); **verified readable by OpenCV** (720×1280×3).
  - **AC3:** 592/592 ctest (+2); media smoke green.
- **Deferred (per user, → TD-09):** AC1's "camera displayed *on a quad*/projector" needs the renderer wired to `SourceManager` (the unconverged dual-model). Capture+PNG — the AI-roadmap-critical part — is complete; on-quad display rides on the deliberate TD-09 convergence.

#### QM-056: AI sidecar skeleton + IPC contract (ADR-035)
- **Status:** DONE — **Completed:** 2026-06-16 (Session 14). Branch `feature/qm-056-ai-sidecar`.
- **Effort:** M · **Depends On:** QM-055
- **Delivered (the boundary, nothing intelligent behind it yet):**
  - **Python sidecar** `tools/ai-sidecar/` — stdlib-only loopback HTTP server; versioned v1 `GET /health` + `POST /surface` (centered stub rect, `confidence 0.0`, `stub:true`). Never linked into `QUADmapper.exe`. 6 GPU-free pytest cases.
  - **C++ `src/ai/SidecarClient`** — attach-or-launch lifecycle; WinHTTP client (Windows system lib, **no vcpkg dep**) behind a pimpl; spawned sidecar in a **Job Object (KILL_ON_JOB_CLOSE)** ⇒ no orphan. Every path degrades gracefully (never throws).
  - **App wiring** — constructed at init, launched **on demand** from a new **AI menu** (Enable / Detect Surface / status), torn down at shutdown. Menu reads "Sidecar not running" when absent; existing workflows untouched. `EditorContext` unchanged.
- **AC met:** AC1 launch/attach/health/teardown no-orphan ✓ · AC2 `/surface` round-trips stub + sidecar-absent disables UI cleanly ✓ · AC3 C++ contract test (4, skip-if-Python-absent) + Python unit test, GPU-free ✓.
- **Gate:** full build clean; **596/596 ctest** (+4); success-metric smoke green (warp + F11 projector, sidecar-absent path). Live launch→health→`/surface`→teardown watched in the contract test (pid spawned + killed).
- **Follow-up:** TD-10 (async `start()` to avoid the ~1–2.5s UI freeze on enable). In-GUI menu click-path not yet watched working (API verified by the contract test).

#### QM-057: Sidecar surface service (Metric3D → plane → 4 corners)
- **Status:** DONE — **Completed:** 2026-06-16 (Session 14). Branch `feature/qm-056-ai-sidecar`.
- **Effort:** M · **Depends On:** QM-056
- **Delivered (pure Python, no app/build impact):** `tools/ai-sidecar/surface.py` — `image → Metric3D (metric depth + normals) → metric point cloud → Open3D segment_plane → largest planar component → 4 corners → confidence`. Metric depth fixes the spike's affine bow. Confidence = planar-quality × **verticality gate** (floor/ceiling → ~0) × **ambiguity gate** (cluttered scene). `/surface` runs it lazily when `image_path` is readable + the model is present, else falls back to the stub (graceful).
- **AC met:** AC1 corners + plane + confidence via Metric3D + Open3D ✓ · AC2 (3 spike scenes, RTX 3080): screen **0.58** encloses the screen, facade **0.59**, gallery floor-trap **0.00** (low, not confidently wrong) ✓ · AC3 **~350ms** steady-state GPU (first call ~7–8s model load); CPU path documented ✓.
- **Verified:** 5 skip-if-model-absent pytest (`test_surface.py`); live HTTP `/surface` round-trip (stub=false, real corners); C++ contract test still green (stub path). Model from local ComfyUI controlnet_aux (env-overridable `QM_M3D_*`).
- **Note:** corner extraction is a propose-then-confirm proposal (e.g. screen BL drags into the stage) — QM-058 adds the human nudge. Deps added to Python 3.11: open3d, addict, yapf (+existing torch/cv2/numpy/einops/matplotlib).

#### QM-058: "Propose corners" UI — human-in-the-loop confirm
- **Status:** DONE — **Completed:** 2026-06-16 (Session 14). Branch `feature/qm-056-ai-sidecar`.
- **Effort:** M · **Depends On:** QM-057
- **Delivered:** AI menu "Detect Surface" → `Application::aiDetectSurface()`: capture a frame (live camera via the QM-055 rail, or `QM_AI_TEST_IMAGE` for a phone-photo-from-disk) → sidecar `/surface` → proposed corners + confidence. Confident (≥0.35) → `setCorners()` on the selected quad (geometry only, no `.qmap` change) → ordinary warp refined with the existing Tab-cycle + arrow-nudge UX. Status via the existing `statusMessage` overlay.
- **AC met:** AC1 capture→sidecar→apply proposed quad (or nudge) ✓ · AC2 low confidence → "tap the surface", no auto-apply ✓ · AC3 applied warp is F11-projectable, smoke green, no `.qmap` change ✓.
- **Verified end-to-end** (`drive_ai_smoke.ps1` via the `QM_AI_SELFTEST` headless hook): screen → "surface applied (0.63)", quad snaps to a perspective trapezoid + F11 projects it (screenshots); gallery floor-trap → "low confidence (0.00) — tap the surface", quad unchanged + project not modified. Success-metric smoke green; 596/596 ctest.
- **Notes:** `QM_AI_SELFTEST`/`QM_AI_TEST_IMAGE` are guarded verification seams (off in normal runs). Detect blocks the UI ~1–3s (pre-show; async → TD-10). Phase 1 of the AI wedge now ships.

#### QM-059: Projector test-pattern emit + single-photo capture flow
- **Status:** DONE — **Completed:** 2026-06-16 (Session 14). Branch `feature/qm-056-ai-sidecar`.
- **Effort:** S · **Depends On:** QM-055
- **Delivered:** `resources/patterns/charuco.png` + `.json` (7×4 ChArUco, DICT_5X5_100, gen by `tools/ai-sidecar/gen_pattern.py`, 3-channel → white-on-black). `OutputManager::renderPatternToOutputs()` draws one fullscreen identity quad to all outputs (additive; scene path untouched). AI → One-Flash menu (Emit / Capture / Clear); emit flashes the pattern on the projector via `renderToProjectorOutputs` when armed; capture grabs a camera frame to disk via the shared `captureCameraFrameToPng()` helper (extracted from `aiDetectSurface`, de-duped).
- **AC met:** AC1 ChArUco flashes fullscreen on the chosen output ✓ · AC2 single capture round-trips to disk (720×1280×3, OpenCV-readable) ✓ · smoke green ✓.
- **Verified:** `drive_ai_smoke.ps1 -SelfTestMode oneflash` — pattern white-on-black fullscreen on the projector (screenshot) + `quadmapper_oneflash_capture.png` on disk. Success-metric smoke green; 596/596 ctest; QM-058 unaffected.

#### QM-060: One-Flash homography solve → push warp
- **Status:** DONE (software-complete) — **Completed:** 2026-06-16 (Session 14). Branch `feature/qm-056-ai-sidecar`. Real-projection alignment is the 🚩 QM-061 gate.
- **Effort:** M · **Depends On:** QM-059, QM-057
- **Delivered:** `oneflash.py` — detect ChArUco in reference + photo, match by id → homography `H` (projector-norm ↔ photo px); target surface corners (QM-057 or passed) → `H⁻¹` → projector-norm quad corners; confidence from inlier ratio + reprojection RMS (cv2.aruco only, GPU-free). `POST /oneflash` route (surface target via QM-057 or full-frame fallback; stub on failure). `SidecarClient::requestOneFlash()` (shared `parseCornersResponse`). App: AI → One-Flash → **Solve & Apply** → apply corners to selected quad (geometry only, no `.qmap` change).
- **AC met (software):** AC1 `/oneflash` returns homography as 4 corner offsets ✓ · AC2 applying warps the quad ✓ (the *physical* "lands aligned on surface" claim is QM-061) · AC3 geometry-only, `.qmap`-safe, smoke green ✓.
- **Verified without hardware:** synthetic known-homography test recovers corners to **<0.02** (conf 0.99, 0.13px RMS); HTTP round-trip; in-app Solve & Apply via `QM_AI_SELFTEST=solve` (conf 0.99, watched). 14/14 sidecar pytest; 596/596 ctest.

#### QM-061: 🚩 Real-device validation gate (Phase 2 not "done" until this passes)
- **Status:** TODO · **Effort:** S · **Depends On:** QM-060
- **AC:** on a real projector + real surface + real phone photo, One-Flash produces a visibly correct warp, watched working; failure modes documented. Fallback if it fails: port MIT gray-code structured-light behind the same sidecar contract.
- **Runbook (Session 15):** turnkey step-by-step procedure + result template at
  [docs/QM-061-hardware-test-runbook.md](docs/QM-061-hardware-test-runbook.md). Note: One-Flash
  itself is GPU-free (`cv2.aruco` only) — QM-061 does **not** need the Metric3D model.
- **On pass:** open the `dev`→`master` PR using the prepared body
  [docs/QM-061-pr-prep.md](docs/QM-061-pr-prep.md).

#### QM-062: Audience-perspective ("trap-art") anamorphic warp (Phase 3)
- **Status:** DONE (software-complete) — **Completed:** 2026-06-16 (Session 14). Branch `feature/qm-056-ai-sidecar`. "Looks correct from the viewpoint" is perspective-validated on real hardware (with QM-061).
- **Effort:** M · **Depends On:** QM-057 (realized via the One-Flash homography, QM-060)
- **Delivered:** AI → One-Flash → **Anamorphic Warp**. Reuses the One-Flash homography: maps a centered **audience** rectangle back through `H⁻¹` → projector corners that make content read frontal from the captured viewpoint. `requestOneFlash()` gained an optional target; `oneFlashSolveAndApply` (surface target) + `oneFlashAnamorphic` (audience target) share `runOneFlash()`. No `.qmap` change.
- **AC met (software):** AC1 frontal-from-viewpoint warp applied ✓ (the perspective claim is hardware-validated) · AC2 geometry-only, `.qmap`-safe, smoke green ✓.
- **Verified:** anamorphic round-trip test (audience target → corners → forward `H` lands back on target, <2px); in-app Anamorphic Warp via `QM_AI_SELFTEST=anamorphic` (conf 0.99, watched). 15/15 sidecar pytest; 596/596 ctest.
- **Note:** a projector-off (QM-057-only) anamorphic is geometrically underdetermined (the spike's finding); this is the One-Flash-based (projector-pose-observed) realization.

---

## In Progress

*No tasks currently in progress. Next: QM-056 (AI sidecar skeleton).*

## Done

#### QM-054: TD-02 — decompose render() into panels (ADR-034)
- **Status:** DONE (6 of 6 batches) — **Completed:** 2026-06-16
- **Notes:** ADR-034: decomposed the ~1900-line `render()` into panel classes over the live `EditorState` model (behavior-preserving), via a per-frame `EditorContext` bundle. All on `refactor/app-decomposition`, each build-clean + 590/590 ctest + smoke-green (incl. F11 projector output): batch 0 — `EditorContext.hpp` + `Application::{newProject,saveProject,loadProject}` (de-dup 3× save/load); batch 1 `MainMenuBar` (708d267); batch 2 `QuadListPanel` (fb23972); batch 3 `InspectorPanel` = context-sensitive Properties (6d522af); batch 3b `VideoPlayerPanel` (3a13950); **batch 4 (last, success-metric-critical) `ViewportEditor`** — the pre-`ImGui::NewFrame()` on-canvas warp surface (quad render + corner/mesh warp + keyboard-capture guard) extracted **byte-for-byte** into a non-`Panel` class; dead `PreviewPanel` shell deleted (f2dd41f). `Application.cpp` 2411→~960 lines; `render()` ~1910→~400. Verified: build clean, 590/590, smoke harness 05 (Tab-cycle + arrow-nudge warp sent fullscreen to the projector), fresh-context Opus verifier APPROVE. Source-arch convergence tracked separately as TD-09.

## Done

### Phase 8: ADR-031 Refactor (Session 10 — 2026-06-16)

#### QM-053: TD-02 step 2 — retire dual-track scene state (one scene of truth)
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** ADR-033. Deleted the unwired multi-scene machinery (`SceneManager` +35 tests, `ScenePanel`, scene-based `ProjectSerializer` methods, orphan `SceneManager` hooks in Inspector/Preview/Renderer, `m_sceneManager`). `EditorState::scene` is now the single source of truth. Reconciled render()'s 5 hand-rolled save/load sync sites into two shared helpers (`syncSceneToProject`/`syncProjectToScene`) using whole-geometry/whole-texture copies — fixing a latent bug that dropped mesh-edit vertex offsets + flipH/V on save/load. Build clean; 589/590 tests (1 pre-existing flaky timer test); success metric harness-verified (vertex warp + F11 projector output). Remaining TD-02 work: decompose render() UI into real panels.

#### QM-052: TD-05 — `.qmap` versioning, migration & save/load de-dup
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** ADR-032. One `validateVersion()` policy across all 4 read paths (accept major ≤ 2, reject newer majors loudly — scene paths previously ignored version). v1.x→single-scene migration; v2.x→flat-model flatten. Collapsed the 4× quad-copy into `loadQuadInto()` (whole-geometry copy), which also fixed a silent mesh-offset drop on scene load. No on-disk change. 631/631 tests (+7 new).

### Phase 8: ADR-031 Refactor (Session 9 — 2026-06-16)

#### QM-045: ADR-031 audit + decision + UI smoke harness
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** 5-specialist audit → refactor-in-place (not rebuild). Added `tests/smoke/drive_smoke.ps1` (drives the real app via Win32 scancode input, screenshots monitors) as the verification gate.

#### QM-046: TD-02 step 1 — extract render() statics → owned EditorState
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** ~40 function-statics → `src/app/EditorState.hpp`; body unchanged via same-named aliases. Behavior-preserved (harness + 624 tests).

#### QM-047: TD-01 — projector output renders the warped scene (SUCCESS METRIC)
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** Per-output `Renderer`; `renderToOutputs(quadManager, textures)` from `run()`; F11 auto-creates an output on the secondary monitor. Was a never-called gray-clear stub. Full success metric verified end-to-end.

#### QM-048: TD-04 — atomic cross-thread video decoder state
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** `currentFrame_`/`currentTime_`/`atEnd_` → `std::atomic` (fixes UB).

#### QM-049: TD-03 — lock-free + alloc-free audio RT callback
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** SPSC ring (`AudioBuffer`), atomic source/callback snapshots (`AudioMixer`), preallocated buffer (`AudioSource`). Residual reclamation nit → TD-08.

#### QM-050: TD-06 — delete dead code
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** Removed `FrameBuffer`/`DoubleBuffer`/`AudioEngine`/`UIManager`/renderer `Framebuffer`+`TextureCache` (files + CMake + wiring).

#### QM-051: Fix full-build regressions (AudioBuffer capacity, test signature)
- **Status:** DONE — **Completed:** 2026-06-16
- **Notes:** Caught on full rebuild (LESSON-BUILD-007: full-build before ctest). AudioBuffer now honors exact capacity; OutputManagerTest updated to new signature.

### Phase 1: Core Foundation

#### QM-001: Setup CMake build system
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Medium
- **Depends On:** None
- **Completed:** 2025-12-19
- **Notes:**
  - vcpkg integration working (glfw3, glm, nlohmann-json)
  - Debug build successful (QUADmapper.exe)
  - Core tests passing (27/27)
  - External libs installed: GLAD, Dear ImGui (docking), stb_image

#### QM-002: Create GLFW window
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Small
- **Depends On:** QM-001
- **Completed:** 2025-12-19
- **Notes:**
  - 1920x1080 window with vsync
  - Framebuffer resize callback
  - Full main loop implementation

#### QM-003: Initialize OpenGL context
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Small
- **Depends On:** QM-002
- **Completed:** 2025-12-19
- **Notes:**
  - GLAD loader initialization working
  - OpenGL 3.3 core profile confirmed
  - Dear ImGui 1.92.6 with docking support
  - Multi-viewport enabled

#### QM-004: Implement single quad with 4-corner warping
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Large
- **Depends On:** QM-003
- **Completed:** 2025-12-19
- **Notes:**
  - QuadGeometry with bilinear interpolation
  - ShaderProgram class for OpenGL shaders
  - Renderer with mesh subdivision support (2-64 resolution)
  - Real-time corner warping

#### QM-005: Add image texture loading
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Medium
- **Depends On:** QM-003
- **Completed:** 2025-12-19
- **Notes:**
  - Texture class with stb_image integration
  - Support for PNG, JPG, TIF, BMP, TGA
  - GPU texture upload with mipmaps
  - UI for loading textures by path

#### QM-006: Create basic ImGui inspector panel
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Medium
- **Depends On:** QM-002
- **Completed:** 2025-12-19
- **Notes:**
  - "Quad Inspector" panel with corner controls
  - Mesh resolution slider (2-64)
  - Opacity control
  - Texture path input and load button

#### QM-007: Implement minimal project save/load
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 1
- **Estimated Effort:** Medium
- **Depends On:** QM-004, QM-005
- **Completed:** 2025-12-19
- **Notes:**
  - JSON format using nlohmann/json (.qmap files)
  - Saves quad corners, mesh resolution, opacity, texture paths
  - File menu with Save/Load and Ctrl+S/Ctrl+O shortcuts
  - Project panel with status display
  - Round-trip serialization verified with unit tests

### Phase 2: Mesh & Multi-Quad

#### QM-008: Implement mesh subdivision system
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 2
- **Estimated Effort:** Large
- **Depends On:** QM-004
- **Completed:** 2025-12-19
- **Notes:**
  - Configurable resolution (2-64)
  - Vertex buffer generation with proper vertex layout
  - Index buffer for triangle list rendering
  - MeshSubdivision utility class with static methods
  - Integration with QuadGeometry for mesh offsets

#### QM-009: Add mesh vertex editing mode
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 2
- **Estimated Effort:** Large
- **Depends On:** QM-008
- **Completed:** 2025-12-19
- **Notes:**
  - Vertex handle rendering with dedicated shaders
  - Drag interaction with mouse click/drag
  - Reset to bilinear option via UI button
  - MeshMode enum (CornersOnly vs MeshEdit)
  - Per-vertex offset storage in QuadGeometry

#### QM-010: Support multiple quads
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 2
- **Estimated Effort:** Medium
- **Depends On:** QM-004
- **Completed:** 2025-12-19
- **Notes:**
  - QuadManager class with full lifecycle management
  - Selection system with single/multi-select (Ctrl+click)
  - Select all (Ctrl+A), range select (Shift+click)
  - Selection callback support
  - Duplicate quad functionality

#### QM-011: Implement quad groups
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 2
- **Estimated Effort:** Medium
- **Depends On:** QM-010
- **Completed:** 2025-12-19
- **Notes:**
  - QuadGroup class with member ID list
  - Group visibility/lock propagation to members
  - Scene API for group management
  - Create group from selection
  - Drag-drop quads to groups in UI
  - 39 unit tests for group functionality

#### QM-012: Add Z-order management
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 2
- **Estimated Effort:** Small
- **Depends On:** QM-010
- **Completed:** 2025-12-19
- **Notes:**
  - Render order by z-order (lowest first)
  - QuadManager z-order operations (up/down/top/bottom)
  - Multi-select z-order operations
  - Keyboard shortcuts: Ctrl+]/[ (up/down), Ctrl+Shift+]/[ (top/bottom)
  - UI controls in Quad List panel

#### QM-013: Implement UV offset/scale controls
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 2
- **Estimated Effort:** Medium
- **Depends On:** QM-005
- **Completed:** 2025-12-19
- **Notes:**
  - UV offset (pan) with X/Y controls
  - UV scale (zoom) with X/Y controls
  - UV rotation support
  - Reset UV button in inspector
  - TextureBinding class for UV state
  - Shader support for UV transforms

### Phase 3: Video Playback

#### QM-014: Integrate FFmpeg video decoder
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 3
- **Estimated Effort:** Large
- **Depends On:** QM-001
- **Completed:** 2025-12-19
- **Notes:**
  - VideoDecoder class with pimpl pattern
  - FFmpeg libavcodec/libavformat integration
  - Background decoder thread with ring buffer
  - RGB24 frame output
  - Seek and frame navigation
  - FFmpeg 7.1.2 via vcpkg

#### QM-015: Implement video texture binding
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 3
- **Estimated Effort:** Medium
- **Depends On:** QM-014, QM-005
- **Completed:** 2025-12-19
- **Notes:**
  - FrameBuffer thread-safe ring buffer
  - Blocking/non-blocking push/pop operations
  - End of stream signaling
  - GPU texture upload with glTexSubImage2D
  - Texture wrapper for external GL textures

#### QM-016: Add playback controls
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 3
- **Estimated Effort:** Medium
- **Depends On:** QM-015
- **Completed:** 2025-12-19
- **Notes:**
  - VideoPlayer class with full controls
  - Play, pause, stop, togglePlayPause
  - Seek by time, progress, skip
  - Video Player UI panel with controls

#### QM-017: Implement speed control
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 3
- **Estimated Effort:** Small
- **Depends On:** QM-016
- **Completed:** 2025-12-19
- **Notes:**
  - Speed range 0.1x to 4.0x
  - Frame skipping for fast speeds
  - Speed slider in UI with 1x reset

#### QM-018: Add loop modes
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 3
- **Estimated Effort:** Small
- **Depends On:** QM-016
- **Completed:** 2025-12-19
- **Notes:**
  - LoopMode enum: Once, Loop, Ping-Pong
  - Loop mode combo box in UI
  - handleEndOfVideo with mode switching

#### QM-019: Implement timeline lock system
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 3
- **Estimated Effort:** Medium
- **Depends On:** QM-018
- **Completed:** 2025-12-19
- **Notes:**
  - TimelineSyncManager class
  - Lock/unlock players to timeline
  - Wait for all locked videos to finish
  - Synchronized restart

---

## In Progress

*No tasks currently in progress*

---

## Todo

### Phase 4: Multi-Output

#### QM-020: Create output window system
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 4
- **Estimated Effort:** Large
- **Depends On:** QM-003
- **Completed:** 2025-12-19
- **Notes:**
  - OutputWindow class with GLFW window management
  - Shared OpenGL context via create(GLFWwindow* shareContext)
  - Fullscreen/windowed toggle with saved position
  - OutputManager with monitor enumeration (MonitorInfo struct)
  - Perform mode enter/exit
  - 50 unit tests (27 OutputWindowTest + 22 OutputManagerTest + MonitorInfo)

#### QM-021: Add resolution/rotation controls
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 4
- **Estimated Effort:** Medium
- **Depends On:** QM-020
- **Completed:** 2025-12-19
- **Notes:**
  - ResolutionPreset struct with common presets (720p, 1080p, 4K, etc.)
  - getEffectiveResolution() accounts for rotation (swaps w/h for 90/270)
  - getRotationDegrees()/setRotationDegrees() conversion methods
  - applyResolution() applies to GLFW window

#### QM-022: Implement monitor assignment
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 4
- **Estimated Effort:** Medium
- **Depends On:** QM-020
- **Completed:** 2025-12-19
- **Notes:**
  - MonitorInfo::getDisplayString() for UI-friendly display
  - getMonitor(index), getPrimaryMonitor() methods
  - MonitorChangeCallback with setMonitorChangeCallback()
  - enableMonitorChangeDetection() with GLFW callback

#### QM-023: Add Perform Mode
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 4
- **Estimated Effort:** Medium
- **Depends On:** QM-022
- **Completed:** 2025-12-19
- **Notes:**
  - goBorderlessFullscreen() for borderless mode
  - KeyCallback with setKeyCallback() on OutputWindow
  - handleF11Key(), handleEscapeKey(), togglePerformMode()
  - setupPerformModeKeyCallbacks() for automatic handling

---

### Phase 5: Scene System

#### QM-024: Implement scene data structure
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 5
- **Estimated Effort:** Medium
- **Depends On:** QM-010
- **Completed:** 2025-12-19
- **Notes:**
  - SceneViewState struct (panOffset, zoomLevel, gridVisible, snapToGrid)
  - Scene::getViewState(), resetView() methods
  - SceneManager: getSceneByIndex, duplicateScene, generateUniqueSceneName, reset
  - ProjectSerializer: saveWithScenes/loadToSceneManager with v2.0 format
  - Backward compatibility for v1.x project files
  - 21 new SceneManager unit tests

#### QM-025: Add scene creation/deletion
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 5
- **Estimated Effort:** Small
- **Depends On:** QM-024
- **Completed:** 2025-12-19
- **Notes:**
  - canDeleteScene() safety check (can't delete last scene)
  - getActiveSceneId(), getSceneIndex() methods
  - setActiveSceneByIndex() for keyboard shortcuts 1-9
  - moveScene() for drag-to-reorder

#### QM-026: Implement scene switching
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 5
- **Estimated Effort:** Medium
- **Depends On:** QM-025
- **Completed:** 2025-12-19
- **Notes:**
  - Instant switch via setActiveScene(UUID)
  - setActiveSceneByIndex() for keyboard 1-9
  - Click-to-select in ScenePanel UI

#### QM-027: Create scene panel UI
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 5
- **Estimated Effort:** Medium
- **Depends On:** QM-026
- **Completed:** 2025-12-19
- **Notes:**
  - ScenePanel.hpp/cpp with full ImGui implementation
  - New/Duplicate/Delete buttons with disabled states
  - Active scene indicator (green ">")
  - Inline rename (double-click), context menu
  - Drag-to-reorder with ImGui drag-drop

---

### Phase 6: Audio System

#### QM-028: Extract audio from video
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 6
- **Estimated Effort:** Medium
- **Depends On:** QM-014
- **Completed:** 2025-12-19
- **Notes:**
  - AudioFrame struct (interleaved float samples, channels, sampleRate, pts)
  - AudioInfo struct for stream metadata
  - AudioBuffer thread-safe ring buffer with blocking/non-blocking ops
  - AudioDecoder with pimpl pattern for FFmpeg isolation
  - SwrContext for converting to float format
  - Background decoding thread support
  - 54 audio tests (17 AudioFrame + 21 AudioBuffer + 16 AudioDecoder)

#### QM-029: Implement audio mixer
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 6
- **Estimated Effort:** Large
- **Depends On:** QM-028
- **Completed:** 2025-12-19
- **Notes:**
  - AudioMixer.hpp/cpp with miniaudio backend
  - Thread-safe mixing of multiple AudioSources
  - Master volume (linear and dB), master mute
  - Source management (add/remove/get/clear)
  - Pimpl pattern isolates miniaudio headers

#### QM-030: Add per-source volume controls
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 6
- **Estimated Effort:** Small
- **Depends On:** QM-029
- **Completed:** 2025-12-19
- **Notes:**
  - AudioSource.hpp/cpp with volume (0.0-1.0) and dB conversion
  - Mute toggle per source
  - Pan control (-1.0 to 1.0) with constant power panning
  - Left/right gain calculation in readSamples()

#### QM-031: Implement multi-output routing (basic)
- **Status:** DONE
- **Priority:** P1
- **Phase:** Phase 6
- **Estimated Effort:** Medium
- **Depends On:** QM-029
- **Completed:** 2025-12-19
- **Notes:**
  - AudioDevice.hpp/cpp with miniaudio device enumeration
  - AudioDeviceInfo struct (id, name, isDefault, channels, sampleRates)
  - Device selection by ID or default
  - Sample rate, channel count, buffer size configuration
  - Pimpl pattern for miniaudio isolation

---

### Phase 7: Polish

#### QM-032: Implement keyboard shortcut system
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-002
- **Completed:** 2025-12-20
- **Notes:**
  - KeyboardShortcuts.hpp/cpp with ShortcutAction enum (51 actions)
  - ModifierFlags (Ctrl, Shift, Alt, Super) with KeyCombination
  - Default Resolume-style bindings, conflict detection
  - JSON save/load to user config directory
  - 52 unit tests

#### QM-033: Create preferences dialog
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-006
- **Completed:** 2025-12-20
- **Notes:**
  - PreferencesDialog.hpp/cpp with 3 tabs (General, Shortcuts, Performance)
  - Interactive shortcut capture with 5s timeout
  - AppPreferences struct with JSON save/load
  - Settings saved to %APPDATA%\QUADmapper\ or ~/.config/quadmapper/

#### QM-034: Add undo/redo system
- **Status:** DONE
- **Priority:** P1
- **Phase:** Phase 7
- **Estimated Effort:** Large
- **Depends On:** QM-010
- **Completed:** 2025-12-20
- **Notes:**
  - CommandHistory.hpp/cpp with execute/undo/redo
  - Command merging for drag operations
  - Compound commands for grouped operations
  - Concrete commands: MoveCorner, MoveMeshVertex, CreateQuad, DeleteQuad, etc.
  - 40 unit tests (17 CommandHistory + 23 Commands)

#### QM-035: Implement media bin panel
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-005, QM-015
- **Completed:** 2025-12-20
- **Notes:**
  - MediaBinPanel.hpp/cpp with thumbnail grid
  - Import button with Windows native file dialog
  - Drag-drop source for quad assignment (MEDIA_ITEM_ID payload)
  - Context menu (Remove, Reveal in Explorer, Copy Path)
  - 24 MediaLibrary tests

#### QM-036: Add preview panel with interaction
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Large
- **Depends On:** QM-004, QM-009
- **Completed:** 2025-12-20
- **Notes:**
  - PreviewPanel.hpp/cpp with real-time scene preview
  - Edit modes: Select, Corners, Mesh
  - Corner/mesh vertex drag editing with handles
  - Zoom (mouse wheel), pan (middle-drag)
  - Grid overlay, selection highlighting

#### QM-037: Create output panel
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-020
- **Completed:** 2025-12-20
- **Notes:**
  - OutputPanel.hpp/cpp with output list
  - Perform Mode button (Enter/Exit) with color state
  - Enable/disable toggles, Add/Remove buttons
  - Monitor dropdown, resolution presets, rotation
  - Inline rename, context menu

#### QM-038: Performance optimization pass
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Large
- **Depends On:** All previous
- **Completed:** 2025-12-20
- **Notes:**
  - PerformanceMonitor.hpp/cpp with frame timing
  - Section timing (render, update, UI phases)
  - Draw call/triangle counting, texture memory tracking
  - FPS history (min/max/avg)
  - ScopedTimer, PERF_SCOPE macros
  - 12 unit tests

#### QM-039: Error handling and logging
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** All previous
- **Completed:** 2025-12-20
- **Notes:**
  - ErrorNotification.hpp/cpp with ErrorNotificationManager
  - Severity levels (Debug, Info, Warning, Error, Critical)
  - Auto-expiring toast notifications (5s default, configurable)
  - Enhanced Logger with severity filtering and file output
  - 18 unit tests

#### QM-040: Create unit tests for core
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-001
- **Completed:** 2025-12-20
- **Notes:**
  - UUIDTest.cpp: 8 tests (generate, copy, assignment, toString, fromString, hashing)
  - ResultTest.cpp: 8 tests (success/error, valueOr, map, andThen)
  - LoggerTest.cpp: 4 tests (level ordering, format strings)
  - TimerTest.cpp: 5 tests (start/stop, elapsed, reset)
  - CommandHistoryTest.cpp: 17 tests (execute, undo/redo, merge, compound)
  - PerformanceMonitorTest.cpp: 12 tests (frame timing, FPS, section timing)
  - ErrorNotificationTest.cpp: 18 tests (notifications, severity, auto-expire)
  - Total: 72 core unit tests

#### QM-041: Create unit tests for scene
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-010
- **Completed:** 2025-12-20
- **Notes:**
  - QuadTest.cpp: 12 tests (creation, properties, visibility, lock)
  - QuadGeometryTest.cpp: 18 tests (corners, bilinear interpolation, mesh offsets)
  - MeshSubdivisionTest.cpp: 15 tests (vertex generation, index generation, resolution)
  - SceneTest.cpp: 22 tests (add/remove quads, groups, visibility)
  - SceneManagerTest.cpp: 35 tests (scene lifecycle, switching, serialization)
  - QuadManagerTest.cpp: 28 tests (selection, z-order, multi-select)
  - QuadGroupTest.cpp: 39 tests (members, visibility propagation, lock)
  - CommandsTest.cpp: 23 tests (move corner, create/delete quad, undo/redo)
  - Total: 192 scene unit tests

#### QM-042: Create unit tests for project
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Medium
- **Depends On:** QM-007
- **Completed:** 2025-12-20
- **Notes:**
  - ProjectTest.cpp: 18 tests (project state, dirty flag, path handling)
  - ProjectSerializerTest.cpp: 32 tests (save/load, v2.0 format, v1.x backward compat)
  - MediaLibraryTest.cpp: 24 tests (add/remove media, thumbnails, metadata)
  - Round-trip serialization verified with multiple scenarios
  - Total: 74 project unit tests

#### QM-043: Create integration tests
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Large
- **Depends On:** QM-040, QM-041, QM-042
- **Completed:** 2025-12-20
- **Notes:**
  - RenderingPipelineTest.cpp: Tests shader compilation, texture binding, mesh rendering
  - ProjectIntegrationTest.cpp: Full project save/load with all components
  - OutputWindowTest.cpp: Window creation, context sharing, fullscreen
  - OutputManagerTest.cpp: Multi-output management, perform mode
  - AudioMixerTest.cpp: Multi-source mixing, volume control
  - AudioDeviceTest.cpp: Device enumeration, output routing
  - KeyboardShortcutsTest.cpp: Shortcut binding, conflict detection
  - Total: 286 integration/component tests

#### QM-044: Manual test protocols
- **Status:** DONE
- **Priority:** P0
- **Phase:** Phase 7
- **Estimated Effort:** Small
- **Depends On:** All previous
- **Completed:** 2025-12-20
- **Notes:**
  - Created docs/MANUAL_TEST_PROTOCOLS.md
  - Visual verification checklist (18 items across 5 categories)
  - Performance benchmark tests (startup, frame rate, memory)
  - Multi-output test procedure (window, perform mode, monitor detection)
  - Scene system tests (management, switching)
  - Audio system tests (playback, device selection)
  - Project save/load tests
  - Input system tests (keyboard, mouse)
  - Error handling tests

---

## Blocked

*No blocked tasks*

---

## Done

*No completed tasks*

---

## Task ID Reference

| ID | Title | Status | Phase |
|----|-------|--------|-------|
| QM-001 | Setup CMake build system | DONE | Phase 1 |
| QM-002 | Create GLFW window | DONE | Phase 1 |
| QM-003 | Initialize OpenGL context | DONE | Phase 1 |
| QM-004 | Implement single quad with 4-corner warping | DONE | Phase 1 |
| QM-005 | Add image texture loading | DONE | Phase 1 |
| QM-006 | Create basic ImGui inspector panel | DONE | Phase 1 |
| QM-007 | Implement minimal project save/load | DONE | Phase 1 |
| QM-008 | Implement mesh subdivision system | DONE | Phase 2 |
| QM-009 | Add mesh vertex editing mode | DONE | Phase 2 |
| QM-010 | Support multiple quads | DONE | Phase 2 |
| QM-011 | Implement quad groups | DONE | Phase 2 |
| QM-012 | Add Z-order management | DONE | Phase 2 |
| QM-013 | Implement UV offset/scale controls | DONE | Phase 2 |
| QM-014 | Integrate FFmpeg video decoder | DONE | Phase 3 |
| QM-015 | Implement video texture binding | DONE | Phase 3 |
| QM-016 | Add playback controls | DONE | Phase 3 |
| QM-017 | Implement speed control | DONE | Phase 3 |
| QM-018 | Add loop modes | DONE | Phase 3 |
| QM-019 | Implement timeline lock system | DONE | Phase 3 |
| QM-020 | Create output window system | DONE | Phase 4 |
| QM-021 | Add resolution/rotation controls | DONE | Phase 4 |
| QM-022 | Implement monitor assignment | DONE | Phase 4 |
| QM-023 | Add Perform Mode | DONE | Phase 4 |
| QM-024 | Implement scene data structure | DONE | Phase 5 |
| QM-025 | Add scene creation/deletion | DONE | Phase 5 |
| QM-026 | Implement scene switching | DONE | Phase 5 |
| QM-027 | Create scene panel UI | DONE | Phase 5 |
| QM-028 | Extract audio from video | DONE | Phase 6 |
| QM-029 | Implement audio mixer | DONE | Phase 6 |
| QM-030 | Add per-source volume controls | DONE | Phase 6 |
| QM-031 | Implement multi-output routing | DONE | Phase 6 |
| QM-032 | Implement keyboard shortcut system | DONE | Phase 7 |
| QM-033 | Create preferences dialog | DONE | Phase 7 |
| QM-034 | Add undo/redo system | DONE | Phase 7 |
| QM-035 | Implement media bin panel | DONE | Phase 7 |
| QM-036 | Add preview panel with interaction | DONE | Phase 7 |
| QM-037 | Create output panel | DONE | Phase 7 |
| QM-038 | Performance optimization pass | DONE | Phase 7 |
| QM-039 | Error handling and logging | DONE | Phase 7 |
| QM-040 | Create unit tests for core | DONE | Phase 7 |
| QM-041 | Create unit tests for scene | DONE | Phase 7 |
| QM-042 | Create unit tests for project | DONE | Phase 7 |
| QM-043 | Create integration tests | DONE | Phase 7 |
| QM-044 | Manual test protocols | DONE | Phase 7 |
| QM-052 | TD-05 .qmap versioning, migration & save/load de-dup | DONE | Phase 8 |
| QM-053 | TD-02 step 2 retire dual-track scene state (one scene of truth) | DONE | Phase 8 |
| QM-054 | TD-02 decompose render() into panels (ADR-034, 6 batches) | DONE | Phase 8 |
| QM-055 | AI pre-show: live camera input + PNG capture (on-quad display → TD-09) | DONE | Phase 9 |
| QM-056 | AI sidecar skeleton + IPC contract (ADR-035) | DONE | Phase 9 |
| QM-057 | Sidecar surface service (Metric3D → plane → corners) | DONE | Phase 9 |
| QM-058 | "Propose corners" UI (human-in-the-loop confirm) | DONE | Phase 9 |
| QM-059 | Projector test-pattern emit + single-photo capture | DONE | Phase 9 |
| QM-060 | One-Flash homography solve → push warp | DONE | Phase 9 |
| QM-061 | 🚩 Real-device validation gate | TODO | Phase 9 |
| QM-062 | Audience-perspective anamorphic warp | DONE | Phase 9 |
| QM-063 | Multi-scene runtime foundation (ADR-039 P1) | DONE | ADR-039 |
| QM-064 | Per-output scene routing + default→2nd monitor (P2) | DONE | ADR-039 |
| QM-065 | Scenes UI: create/switch/assign output (P3) | DONE | ADR-039 |
| QM-066 | Live per-output preview (P4) | DONE | ADR-039 |
| QM-067 | .qmap v3 scenes+outputs persistence (P5) | DONE | ADR-039 |
| QM-068 | Per-quad normalized crop rectangle (P1) | DONE | ADR-040 |
| QM-069 | Renderer reads SourceManager via mediaId (P2, TD-09) | DONE | ADR-040 |
| QM-070 | Shared / multi-quad video — one decode (P3) | DONE | ADR-040 |
| QM-071 | Cross-scene / cross-output source sharing (P4) | DONE | ADR-040 |
| QM-072 | Persist box-wrap (mediaId + crop + typed reload) (P5) | DONE | ADR-040 |

---
