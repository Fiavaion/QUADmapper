# Foundation Audit: 2026-09-25

**Why:** restart after three months away. We want to know how QUADmapper really works, how good it is, and whether it is a sound base for everything beyond "several warped quads from one projector".
**Method:** Capability Evaluation Mode (CEM). Nine persona teams ran in parallel. Every BLOCKER and HIGH finding was then either re-checked by the lead or passed to a fresh-context adversarial verifier (Opus 5.5, high). See [Method & confidence](#method--confidence).
**Branch audited:** `dev` @ `c79ceec`, plus the uncommitted ADR-045 theme work.

---

## 1. Bottom line

The skeleton is good. Several of its core choices are ones a pro media server would make:
- shared-context renderer with one `Renderer` per output
- `TextureSource`/`SourceManager` seam, with uploads on the main thread only
- additive, version-gated `.qmap` format
- view transform shared by rendering and hit-testing in `ViewportEditor`
- lock-free SPSC audio ring

**Nothing needs a rewrite.**

However, the DONE list has run ahead of what actually works on a projector. Green tests (650/654) sit next to confirmed defects that a technician would hit in the first ten minutes:

- **The warp is not perspective-correct.** The corner pin is bilinear, so interior content lands up to **185 px off at 1080p** on a strong keystone, and 154 px on a wall at about 45°. For a projection mapper this is the core promise.
- **The project file is not safe.**
  - Save truncates in place, with no temp file, rename or backup.
  - File > New then Ctrl+S overwrites the last project.
  - There is no unsaved-changes prompt, no autosave, and no top-level exception guard.
  - Procedural bindings are lost on save/load.
- **Undo can use freed memory** after File > New or File > Load. Two teams found this independently and the verifier confirmed it. Every Load also leaves a dangling `QuadManager&` that the rest of that frame reads.
- **The projector shows less than the editor.**
  - Output contexts never enable `GL_BLEND`, so **soft edge and opacity don't appear on the projector**.
  - Output rotation is saved but never applied.
  - Output windows request GL 4.1 against a 3.3 floor.
  - F11 moves keyboard focus to the output window, so nudging stops working.
- **Video plays at the wrong speed.** The player pops at least one frame per loop iteration, so **30 fps content plays at about 2× on a 60 Hz display**.
  - HAP can't be started from the UI.
  - Video quads load black.
  - Unaligned RGB24 uploads can shear the image.
- **Audio doesn't exist at runtime.** `src/audio` (2.4k LOC, 156 tests) is never constructed by the app.
- **The success metric has a hidden extra click.** ImGui keyboard navigation captures Tab and the arrows after "+ Add Quad" until the user clicks empty canvas.

Test effort has gone to the wrong places. The dead audio module has the most tests. `app` + `ui` + `video` (about 11k LOC, including the success-metric code) have **zero**.

Beyond the bugs, three **missing foundations** block every next-tier feature:
1. **No canvas/slice model.** A scene *is* one output's [0,1] space. This blocks multi-projector work, projector blending, layers and crossfades.
2. **No parameter or addressing system, and no stable IDs.** This blocks OSC, MIDI, DMX, cues and effect parameters.
3. **No show-safety layer:** atomic save, autosave, crash guard, perform lock.

**Recommendation:**
- **Phase 0:** make what already exists true. This is about 2 weeks of disjoint slices.
- **Phase 1:** four foundation ADRs, written *before* any new feature.
- **Phase 2:** build features in market order, starting with multi-projector canvas plus edge blending.

---

## 2. Ground truth (run this session)

| Check | Result |
|---|---|
| `cmake --build build/debug` | ✅ exit 0. (The first run was incremental. A later rebuild of `app`/`ui` shows existing MSVC C4996 deprecation warnings for `strncpy`/`getenv`; no other warnings.) |
| `ctest --test-dir build/debug` | 649 pass, 4 skipped, **`stress_smoke` "Not Run"**. The VS multi-config generator needs `-C Debug`, so the documented command is wrong for that test |
| `ctest ... -C Debug -R stress_smoke` | ✅ passes |
| Skipped tests | 4 × `AudioDecoderIntegrationTest`, skipped because no fixture media exists, so the audio decode path is never exercised |
| `master` vs `dev` | `master` is **71 commits behind**. The ADR-040 PR was drafted as a doc (`docs/QM-061-pr-prep.md`) but never opened |
| GUI | Not run in this audit. All interaction findings come from code and were verified by tracing. The Session-20 🚩 visual-verify list is still open |

---

## 3. How it actually works (one page)

```
main → Application (singleton, 2,043 lines)
  run(): poll → SourceManager::updateAll (decode queues → glTexSubImage2D, main ctx)
       → ViewportEditor (pre-NewFrame: draws quads + corner/mesh/bezier handles, Tab/arrow nudge)
       → output preview FBOs (320×180) → ImGui panels → main swap
       → renderToProjectorOutputs: for each output, make-current → draw its scene → swap (sequential)

EditorState (app layer) owns scenes[] ─ Scene{id, assignedOutputId → ≤1 OutputWindow}
   └ QuadManager → Quad{Geometry[corners in output [0,1], grid, meshOffsets, bezier[8]],
                         TextureBinding{mediaId → SourceManager (runtime only), uv, crop, flip},
                         ColorCorrection, SoftEdge, opacity, zOrder}
   └ commandHistory (commands hold raw Quad*)
Project = serialization DTO only; Application.cpp:76-262 copies live ⇄ DTO (untested path)
OutputManager → OutputWindow (GLFW window, context shared with main, own Renderer)
SourceManager → TextureSource: VideoFileSource (1 decode thread, sws→RGB24, 8-frame queue),
                HapFileSource (demux thread → BCn), CameraSource (dshow), Procedural (GL 4.3 compute)
```

- **Warp:** `CORNERS_ONLY` quads are a bilinear or Coons patch tessellated 32×32 on the CPU every draw (`Renderer.cpp:211-243`, `QuadGeometry.cpp:286-349`). `MESH_EDIT` quads are a piecewise-linear control grid, and **the two modes exclude each other**.
- **Success-metric input:** lives in `ViewportEditor.cpp:425-573`, not the `Application.cpp:674-808` that CLAUDE.md cites. The base step is 0.001 normalized units, ×10 with Shift and ×0.1 with Alt. Each nudge is a merging `MoveCornerCommand`.
- **Colour, crop and soft edge:** per-quad uniforms in `quad.frag`. The soft edge is a ramp in the quad's own UV space, **not a projector-overlap blend**.
- **Module graph (real):** `ui` includes `app` 20 times, and `app` includes `ui`, so there is a cycle. `renderer` and `source` include each other's headers. `Application.cpp` has grown back from about 960 to 2,043 lines: about 300 lines are the stress harness, about 250 are AI one-flash code, and `render()` is about 575 lines again.

---

## 4. Verified defects, ranked

Legend: ✔V = confirmed by the adversarial verifier. ✔L = confirmed by the lead. ◐ = one team's finding, not independently re-checked.

### Tier 0: breaks the product promise or loses the user's work

| # | Defect | Evidence | Status |
|---|---|---|---|
| 0.1 | The corner pin is **bilinear, not projective**. Interior error: 185 px at centre on a strong keystone, 26 px at 10%, 154 px on a wall at 45° | `QuadGeometry.cpp:286-291,330`; `quad.vert` has no q/w; the QM-060 homography only sets corners (`Application.cpp:1459,1635`) | ✔V (measured by script) |
| 0.2 | **Undo uses freed memory** after New (1 quad → 1 quad) or reloading the same file. **Every Load** also leaves `quadManager&` (`:770`) dangling for the rest of the frame | Only clear at `Application.cpp:1085`; `newProject` `:1336`; `syncProjectToEditor` `:133/:162`; `GridUndo.hpp:6-7` comment is false | ✔V ×2 teams |
| 0.3 | **Save truncates in place**, with no temp file, rename or `.bak` | `FileSystem.cpp:18-29` | ✔L |
| 0.4 | **New → Ctrl+S overwrites the last project**. `es.projectPath` is never reset. New also keeps the other scenes, sources and outputs | `Application.cpp:1336-1364`, `EditorState.hpp:146` | ✔V |
| 0.5 | No top-level exception guard, no unsaved-changes prompt, and the autosave preference is unused | `main.cpp:16-21`; `MainMenuBar.cpp:52`; `PreferencesDialog.hpp:28` | ✔L / ✔V |
| 0.6 | **Procedural bindings are lost on save/load**, because `mediaId` is never serialized | `ProjectSerializer.cpp:166-176`, `MediaAssign.hpp:29` | ✔V |
| 0.7 | **Delete while typing in a field deletes the selected quads**, with no undo | `Application.cpp:1108-1114` sits outside the `WantTextInput` guard | ✔V |

### Tier 1: the projector doesn't match the editor

| # | Defect | Evidence | Status |
|---|---|---|---|
| 1.1 | `GL_BLEND` is only on in the main context, so **soft edge and opacity are invisible on outputs** | `Application.cpp:459` is the only site | ✔L |
| 1.2 | Output rotation is saved and changes the editor aspect, but is **never applied** to the output | 0 uses in `OutputManager.cpp` or the shaders | ✔L |
| 1.3 | Output windows request **GL 4.1** while the main context uses 3.3. `createOutput` fails on GL 4.0 GPUs, so F11 aborts | `OutputWindow.cpp:101-102` | ✔L |
| 1.4 | F11 moves focus to the output window, whose key callback handles only Esc and F11, so **nudging stops working after F11** | `OutputWindow.cpp:98,144`; `OutputManager.cpp:669-676` | ✔V |
| 1.5 | `NavEnableKeyboard` makes ImGui capture Tab and the arrows after any panel click, including "+ Add Quad", so the success metric needs an extra click | `Application.cpp:479`; imgui 1.92.6 `imgui.cpp:5584`; `ViewportEditor.cpp:427,533` | ✔V |
| 1.6 | Output swaps run in sequence with no swap-interval policy on outputs. With N outputs under vsync the loop can drop to 60/(N+1). The hidden default output is also drawn every frame. **FPS has never been measured** | `OutputManager.cpp:411-453` | ◐ (inferred) |
| 1.7 | Monitor identity is saved as an enumeration index, so outputs land on the wrong screen after displays re-order | `OutputWindow.cpp:199-204,294-297` | ◐ |
| 1.8 | The modal `GetOpenFileNameA` runs on the render thread, so **every projector freezes** while a file dialog is open | `MediaBinPanel.cpp:539`, `VideoPlayerPanel.cpp:51` | ◐ |

### Tier 2: media and playback correctness

| # | Defect | Evidence | Status |
|---|---|---|---|
| 2.1 | At least one frame is popped per loop, so **30 fps plays at about 2× at 60 Hz**, and speeds below 1× have no effect | `VideoPlayer.cpp:173-193`, `HapFileSource.cpp:204-218` | ✔L |
| 2.2 | `GL_UNPACK_ALIGNMENT` is never set, so RGB24 at widths not divisible by 4 shears and over-reads. It only works by accident of ImGui's state | 0 hits in `src/`; `Texture.cpp:240` | ✔L ×2 teams |
| 2.3 | HAP sources are cast to `VideoFileSource`, so **HAP can't be played from the UI** | `MediaAssign.hpp:64`, `VideoPlayerPanel.cpp:109,176` | ◐ |
| 2.4 | Video sources are never played after a project load, so **video quads load black** | `Application.cpp:188-192`, `VideoPlayer.cpp:161` | ◐ |
| 2.5 | The stop flag is set without holding the mutex, a lost-wakeup race, so `join()` can hang the UI on seek, loop or remove | `VideoDecoder.cpp:366-371`, `HapFileSource.cpp:268-272` | ◐ |
| 2.6 | Sources are never released. Orphans that are still playing keep decoding and uploading (a leak) | `SourceManager.cpp:279`, `VideoPlayerPanel.cpp:140` | ◐ |
| 2.7 | Any non-EOF decode error spins the decode thread at 100% of a core and nothing reaches the UI | `VideoDecoder.cpp:302-306,414-419` | ◐ ×2 teams |
| 2.8 | "Ping-Pong" loop mode is just a loop. `TimelineSyncManager` (QM-019) is unreferenced | `VideoPlayer.cpp:235-241` | ◐ |

### Tier 3: structural (costs grow with every feature)

- **Audio is unwired.** It is an MVP feature in PRD §4.1.4 and gets zero references outside `src/audio` (✔L). Either wire it into a master clock (ADR-050) or formally cut it from the MVP.
- **Duplicate Quad drops most of the quad's state:** mesh offsets, bezier, binding, crop, colour and soft edge (✔V). The existing duplicate tests pass while this is broken.
- **Most edits bypass undo:** numeric corners, reset, add/delete/duplicate, opacity, colour, media. A drag's undo also silently reverts later numeric edits.
- **Dead code.** A 562-line `KeyboardShortcuts` registry that nothing reaches. `InputManager::processInput` is commented out. `PreferencesDialog` is never rendered. Stubs include `PerformMode`, `Mesh`, `VertexArray`, `VertexBuffer`, `RenderContext`, `Shader`, and `mesh.vert/frag`. Menu labels promise Ctrl+N, Ctrl+O and Esc, and none of them is bound.
- **Layering.** The `ui` ↔ `app` cycle. `EditorState` holds the live model in the app layer. The live ⇄ DTO mapping sits in `Application.cpp` with no tests.
- **Per-frame costs.** About 42 KB of geometry is rebuilt per quad, per draw, per context, into one shared VBO. `glGetUniformLocation` runs on every set, and `glGenerateMipmap` on every video frame. Decoding is single-threaded software with sws→RGB24 on the CPU and no hwaccel.
- **Procedurals need GL 4.3 compute,** which breaks the stated 3.3 floor.

---

## 5. Claimed vs real

| Claim | Reality |
|---|---|
| "Perspective-corrected quads" (CLAUDE.md) | Bilinear. ADR-043:35 admits it |
| ADR-037 soft edge "verified on projector" | Can't show on outputs, because `GL_BLEND` is off there |
| QM-021 output rotation DONE | Saved and shown in the UI, never applied |
| QM-077 "history cleared on delete/scene switch" makes undo safe | The count heuristic misses New and Load, which leads to use-after-free |
| "Nothing is dropped on round-trip" (`Application.cpp:73`) | `mediaId`/procedural bindings, transport state and active scene are all dropped |
| "Relative paths for portability" (`Project.hpp:68`) | `makeRelative` has no callers, so paths are stored absolute |
| QM-080 HAP "verified end-to-end" | Verified through the stress harness only; the UI can't start HAP |
| QM-081 "leak-clean across perform cycles" | Per-cycle `Renderer` create/destroy isn't on that path, and the "recovered" check can't detect a wedged decoder |
| QM-019 timeline lock DONE | `TimelineSyncManager` is unreferenced |
| `src/audio` "multi-output routing" | Single device, and never constructed |
| ADR-045 "real UI fonts" | The loader already existed. Regular, Medium and Bold are the same variable file, and Mono is loaded but never `PushFont`ed. The recolour itself is fine to commit |
| ADR-043 bezier edges | Implemented, with **zero tests** |
| 60 fps × multi-1080p target | Never measured |
| CLAUDE.md success-metric anchor `Application.cpp:674-808` | Stale. The code is in `ViewportEditor.cpp:425-573` |

---

## 6. Foundation verdict

| Component | Verdict | Note |
|---|---|---|
| Shared-context + Renderer-per-output model | **KEEP** | Right design. Fix the per-context state (blend) and pacing |
| `quad.frag` (colour/crop/soft edge) | KEEP | Premultiply alpha |
| Renderer quad path | REFACTOR | Cached VBO per quad, uniform cache, projective base, blend state |
| OutputManager loop | REFACTOR | Pacing, one render body instead of three copies, stable monitor ID |
| TextureSource / PlayableSource / SourceManager seam | **KEEP** | Add pixel format and stride, refcounted lifetime; the UI should use `PlayableSource` |
| VideoDecoder / VideoPlayer | REFACTOR | Pacing, threads, hwaccel, YUV-in-shader, errors, master clock |
| HapFileSource, CameraSource, MediaLibrary | KEEP | Small fixes |
| Procedural sources | REFACTOR | Move to a GL 3.3 fragment/FBO path |
| TimelineSyncManager, PerformMode stub, dead renderer stubs | REPLACE/DELETE | |
| `.qmap` version policy and additive fields | **KEEP** | Harden input helpers (checked `Vec2`/`Vec3`, tolerant UUIDs, grid sanitising) |
| `FileSystem::writeFile` for projects | REPLACE | Atomic write + `.bak` |
| Quad / QuadGeometry / MeshSubdivision | REFACTOR | One surface evaluator (see ADR-049) |
| Scene ↔ Output coupling | REFACTOR | Becomes Canvas → Slices (see ADR-047) |
| Commands / CommandHistory | KEEP the core, REFACTOR the commands | Key on (sceneId, quadId) |
| ViewportEditor view transform and hit-test | **KEEP** | Solid |
| Input capture layer | REFACTOR | Canvas-focus model, press-edge drags |
| Shortcuts / InputManager / KeyboardShortcuts | REPLACE | One action registry |
| EditorState / panels | REFACTOR | New `editor` module to break the ui↔app cycle; one Panel interface |
| Audio ring and mixer | KEEP the primitives | The integration is greenfield. Fix TD-08 and `readBuffer_` growth first |
| Theme | KEEP | |

---

## 7. Readiness for the next tier

| Target | Status | What's missing |
|---|---|---|
| **T1** multi-projector virtual canvas + projector edge blending | 🔴 blocked | Canvas (pixel space) → per-output slice + output warp + screen-space blend zones; working blend state. The primitives (FBO, shared contexts) exist, so **not a rewrite** |
| **T2** masks, layers, opacity, blend modes | 🟠 | Composite FBO per output, per-draw blend modes, mask entity. Opacity is currently broken on outputs |
| **T3** Spout / NDI / capture / camera | 🟡 | Pixel formats + stride, PBO uploads, saved Source entity, UI to assign a camera. Spout fits the GPU-native path via NV_DX_interop |
| **T4** MIDI / OSC / Art-Net / cues / crossfades | 🔴 blocked | Parameter registry, stable IDs, master clock/transport, a composite stage for crossfades |
| **T5** shader effects / generators (ISF) | 🟠 | GL 3.3 fragment path, per-quad effect chain with parameters |
| **T6** many 1080p/4K videos @ 60 fps, synced | 🔴 blocked | Pacing fix, frame-threaded or hw decode, YUV shader, PBO, no per-frame mipmaps, cached geometry, **measurement** |
| **T7** show reliability | 🔴 blocked | Atomic save, autosave/recovery, crash guard, dirty prompts, perform lock, non-modal dialogs |

**Market context** (from the research team): the table-stakes baseline that every paid tool (MadMapper, Resolume Arena, HeavyM, Millumin) has on day one:
1. edge blending
2. a virtual canvas separate from surface warp
3. OSC + MIDI learn
4. Spout + NDI
5. bezier/polygon masks
6. Art-Net/DMX
7. cue list with transitions
8. autosave and crash recovery

Realistic differentiators for QUADmapper:
- keyboard-first precision warp (multi-vertex select + nudge, numeric pixel readout)
- one-flash camera auto-calibration followed by keyboard refinement (QM-060)
- lean, Windows-native install with HAP
- a diffable `.qmap`

---

## 8. Recommended plan

### Phase 0: make what exists true (about 2 weeks, parallel slices over disjoint files)

Each slice has checkable acceptance criteria and needs a verifier pass plus a user visual-verify to count as done.

| Slice | Contents | Main files | Acceptance |
|---|---|---|---|
| **S1 Show-safety & persistence** | Atomic save (temp → `MoveFileExW`) + `.bak`; top-level catch + emergency save; dirty tracking + prompts on New/Load/Exit; New resets path, all scenes and sources; autosave to a recovery file; v3.1 additive `sources` table + saved `mediaId` + transport state; checked JSON helpers; relative media paths | `core/FileSystem`, `project/`, `Application` New/Load/Save | Kill -9 during save leaves a loadable file; New→Ctrl+S never touches the old file; procedural binding round-trips; fixture `.qmap` v1/v2/v3 files load (the fixtures directory is empty today) |
| **S2 Undo & edit safety** | Commands keyed by (sceneId, quadId), resolved at execute/undo; explicit `commandHistory.clear()` in New/Load; defer New/Load to the frame boundary (fixes the dangling `quadManager&`); `WantTextInput` guard on Delete; undoable delete and duplicate; full-state duplicate | `scene/Commands`, `app/GridUndo`, `QuadManager`, `Application` shortcuts | Tests: undo after New/Load is a no-op, not UB; Delete in a text field doesn't delete; the duplicate matches the original's full state |
| **S3 Projector truth** | Blend state in `Renderer::initialize`; output ctx 3.3; apply rotation; `GLFW_FOCUS_ON_SHOW=FALSE` + return focus to the editor; per-output swap interval policy; stable monitor ID (name + position); non-modal file dialogs off the render thread | `renderer/`, `output/`, `ui` dialogs | **An FBO readback test of an output** shows soft edge and opacity; F11 then Tab and arrows still nudge; FPS overlay with 2 outputs ≥ 59 |
| **S4 Playback correctness** | Pop only when pts ≤ target; `GL_UNPACK_ALIGNMENT` 1; UI uses `PlayableSource`; restore play state on load; stop flag under the mutex; error backoff + state; refcount-GC for sources; real ping-pong or remove it | `video/`, `source/`, `MediaAssign`, `VideoPlayerPanel` | 30 fps clip at 60 Hz plays in real time (test via injected clock); HAP plays from the UI; 854×480 clip renders unsheared |
| **S5 Success metric & warp correctness** | **Projective base**: build the 33×33 tessellation from the homography of the 4 corners (Coons deltas on top for bezier). The tessellation already exists, so this is small. Canvas-focus model instead of NavEnableKeyboard capture; drags start on press only and respect `WantCaptureMouse`; extract nudge/cycle maths into a testable unit | `QuadGeometry`, `ViewportEditor`, `Application` | Keystoned-rect test: centre maps to the diagonal intersection (< 0.5 px); gtest for Tab-cycle and 1/10/0.1 nudge; add quad → Tab works with no extra click |
| **S6 Hygiene** | Fix `stress_smoke` config (`-C Debug` / preset); update the CLAUDE.md anchors and the "Adding a panel" steps; move AI one-flash and `runStress` out of `Application.cpp`; delete dead stubs; **decide on audio** (wire it in S4/ADR-050, or cut it from the MVP); commit ADR-045; open the dev→master PR | docs, `app/`, CMake (needs approval) | `ctest --preset debug` 100% with no "Not Run"; `Application.cpp` < 1,200 lines |

S1 through S5 touch mostly disjoint files and can run in parallel in separate worktrees. S2 and S1 both touch `Application` New/Load, so sequence S1 → S2 for those functions.

### Phase 1: foundation ADRs (written before code; the lead writes them)

(ADR-046 is taken by Phase 0 persistence: [show-safe persistence](../adr/0046-show-safe-persistence.md).)

- **ADR-047 Canvas → Slice → Output.** A virtual canvas in pixel space; surfaces live in canvas space; each output takes one or more slices, each with its own output warp and screen-space edge-blend zones (gamma, width), rendered as canvas FBO → per-output composite. Stable monitor identity. Unblocks T1, T2 and crossfades.
- **ADR-048 Parameter registry & stable addressing.** Every controllable value is a typed parameter `(path, type, range, default)`, e.g. `/scene/{id}/quad/{id}/opacity`. The UI, undo, serializer and later OSC/MIDI/DMX/cues all bind to it. Saved IDs are adopted on load. Unblocks T4 and T5.
- **ADR-049 Unified surface evaluator.** Homography base + control grid + optional tangents, with render density independent of control density. This ends the `CORNERS_ONLY`/`MESH_EDIT` split and **is the prerequisite for QM-079** (bezier grid lines, which you wanted next).
- **ADR-050 Media pipeline v2.** A master clock and transport shared by video and audio, PBO uploads, YUV→RGB in the shader, frame-threaded decode + d3d11va, pixel formats with stride (for NDI and capture), refcounted sources, audio wired into the clock.
- **ADR-051 Editor architecture.** An `editor` module (EditorState + mutations) that breaks the ui↔app cycle, one Panel interface, an action registry replacing hard-coded shortcuts and the dead `KeyboardShortcuts`, and a perform lock.

### Phase 2: features, in order

1. **Multi-projector canvas + edge blending** (ADR-047): your stated next capability, and the biggest competitive gap.
2. **OSC + MIDI learn** (ADR-048): cheap once parameters exist, and plugs into any show rig.
3. **Spout + NDI** inputs (ADR-050 formats).
4. **Masks + layers + blend modes** on the composite stage.
5. **Cue list + crossfades**, then LTC/MTC timecode.
6. **Art-Net/DMX** in and out through the parameter registry.
7. **Ship camera auto-calibration** (the QM-061 hardware gate; write the solve into the ADR-049 evaluator).
8. **ISF/GLSL generators** on the GL 3.3 fragment path.

---

## 9. Using Opus 5.5 for the build (lessons from this run)

- **Opus-tier specialist personas found nearly every BLOCKER and HIGH.** The four teams were data model, render, media and UX.
  - The project specialist agents default to `model: sonnet`. They were overridden to Opus for this audit.
  - **Suggestion:** set `renderer-specialist`, `video-specialist` and `ui-specialist` to Opus in their frontmatter. They run at medium effort, which is Anthropic's default.
- **Sonnet scans were accurate but shallow.**
  - The claims auditor sampled 13 of about 30 claims.
  - The market analyst guessed QUADmapper's own column (it marked masks as present).
  - Use Sonnet for enumeration; not for judgement.
- **Overlapping scopes on purpose paid off.** Where two teams found the same defect independently (undo use-after-free, unpack alignment, decode spin), confidence was high before verification. Give critical areas two teams.
- **The high-effort verifier was worth its cost.** It refuted nothing false, found a worse variant of the top bug, demoted one half-claim, and turned an assertion into a measurement (the 185 px warp error).
- **Phase 0 routing:**
  - The lead (Opus high) writes the S-slice acceptance tests and ADR-047..051 inline.
  - `implement` agents (Opus medium) take S1 to S5 in separate worktrees, in parallel.
  - `verify` (Opus high) gates each slice against its acceptance column.
  - Sonnet writes regression tests from the verified repros above.
  - `chore` handles S6 hygiene.
  - Fable is not needed.

---

## Method & confidence

| Team (persona) | Agent · model | Output |
|---|---|---|
| Lead: build/ctest ground truth, module graph, `Application` structure, synthesis | Opus 5.5 high (CEM) | §2, §3, spot-checks marked ✔L |
| Render & Output architect | renderer-specialist → Opus 5.5 | 0 B / 6 H / 8 M / 5 L |
| Media & Playback engineer | video-specialist → Opus 5.5 | 0 / 9 / 8 / 3 |
| UX & Interaction designer | ui-specialist → Opus 5.5 | 2 / 5 / 6 / 2 |
| Data model & Persistence architect | cpp-executor → Opus 5.5 | 2 / 8 / 7 / 3 |
| Audio engineer | audio-specialist · Sonnet | 1 / 3 / 3 / 2 (the lead re-rated the blocker to HIGH: not on the success-metric path) |
| Test-suite auditor | scan-sonnet · Sonnet med | 5 H / 4 M / 5 L |
| Robustness/security scanner | scan-sonnet · Sonnet med | 2 B / 1 H / 2 M / 1 L |
| Claims-vs-code auditor | scan-sonnet · Sonnet med | 5 backed / 6 partial / 1 not found (sampled) |
| Market analyst | research · Sonnet med | capability matrix, top 8, build order (cited) |
| Adversarial verifier | verify · Opus 5.5 high | V1–V8: 7 confirmed (1 worse than claimed), 1 partial |

**Not verified:** nothing was driven in the GUI. Items marked ◐ are single-team findings from reading code. Most are cheap to confirm while doing the S3/S4 slices. The 60 fps and multi-output pacing claims are inferred, and no FPS measurement exists yet.
