# UI Smoke Harness

`drive_smoke.ps1` is the verification gate for the [ADR-031](../../docs/adr/0031-keep-vs-rebuild.md)
refactor: it drives the **real** app to exercise the success-metric workflow that unit
tests can't reach, then captures screenshots for visual + numeric verification.

Two sibling harnesses close the gap that `drive_smoke.ps1` left (it warps a **white** quad
and never assigns media):

- **`drive_media_smoke.ps1`** — loads a **video** via the Video Player panel, assigns it to
  the quad, plays it, warps a corner, and sends it to the projector (F11). Verifies the full
  FFmpeg-decode → `quadTextures`/video-wrapper → viewport **and** projector path.
  `-VideoPath <file>` (default a local test clip); output in `out_media/`.
- **`drive_image_smoke.ps1`** — loads a still **image** via the Inspector "Texture: Path/Load"
  field, warps a corner, sends it to the projector. `-ImagePath <file>`; output in `out_image/`.
- **`drive_ai_smoke.ps1`** — AI "Detect Surface" (QM-058). Pre-warms the sidecar, launches the
  app with the `QM_AI_SELFTEST` hook so `aiDetectSurface()` runs headlessly on the default quad
  (avoids fragile ImGui menu-driving), then asserts via the app log + screenshots. `-TestImage`
  picks the scene: `test_screen.png` → "surface applied" + F11 (AC1/AC3); `test_wall.png` →
  "low confidence … tap the surface", no apply (AC2). Output in `out_ai/`. The `QM_AI_SELFTEST` /
  `QM_AI_TEST_IMAGE` env vars are off in normal runs (verification seams).

- **`qa_lib.ps1`** — helpers for step-by-step agent/manual QA: dot-source it in each shell call
  (`. tests/smoke/qa_lib.ps1`), then `Start-Qa`, `Click`, `Drag`, `Tap`, `Chord`, `Paste`, `Cap`
  (`-Virtual` for all monitors) against the running app; output in `out_qa/`.

- **`drive_3d_smoke.ps1`** — step-by-step helpers for the ADR-052 3D feature (dot-source it; it
  loads `qa_lib.ps1`). `New-Qm3dAssets` writes a 1-unit cube OBJ with one labelled atlas cell per
  face (each with an "UP" bar) to `%TEMP%\qm_3d`; `Initialize-Qm3dScene` imports it, assigns the
  atlas and turns it; `Submit-QaFileDialog` fills native Open/Save dialogs by window message;
  `Get-QaSilhouette` / `Get-QaModelSilhouette` measure an object's outline corners (for placing
  calibration points and comparing renders). Its `Tap`/`Chord`/`Click` re-verify the foreground
  window before every input, because another app on the desktop can steal it mid-run.

The harnesses place the editor window at 0,0 2560x1360 on the primary monitor (a maximized
window covers the taskbar area here, hiding the bottom of the dock panels); all click points
are physical pixels for that placement. Older notes below mention maximizing. Both (`SW_MAXIMIZE`) and paste the media path via the clipboard
(`Ctrl+V` into the ImGui `InputText`); their deeper-panel click points (Assign/Play, Texture
row) are tunable params — read the early `*_loaded`/`*_typed` capture to retune if the docked
layout or screen size changes.

## Run

```powershell
pwsh -File tests/smoke/drive_smoke.ps1
# or: powershell -File tests/smoke/drive_smoke.ps1
```

Prereq: build the app first — `cmake --build build/debug --target QUADmapper`.

## What it does

1. Launches `QUADmapper.exe`, brings it to the foreground (Win32).
2. Clicks the passthru central canvas to clear ImGui nav/widget focus (so Tab-cycle-vertex
   wins over ImGui keyboard nav).
3. Drives the CORNERS_ONLY vertex workflow with real keyboard input:
   `{TAB}` cycles corners, Shift+Arrow = 10× nudge (0.01/press), plain Arrow = 1× (0.001/press).
4. Screenshots the primary monitor into `out/` after each step; logs app stdout/stderr there too.
5. Presses **F11** to send the warped scene fullscreen to the projector (F11 auto-creates an
   output on the secondary monitor + enters perform mode), then captures the **whole virtual
   desktop** (`05_projector_output.png`) so the projector content is visible regardless of
   per-monitor DPI scaling (a single-monitor capture crops under scaling).

## Verify

Read the PNGs in `out/` (gitignored). The Properties panel shows the live corner values —
check them against the expected math. Known-good baseline (default quad 0.3–0.7):

| Step | Action | Expected corner |
|------|--------|-----------------|
| 02 | corner0: Shift+Right×20, Shift+Down×10 | Top Left 0.500, 0.400 |
| 03 | corner1: Shift+Left×15 | Top Right 0.550, 0.300 |
| 04 | corner2: plain Up×20 | Bottom Right 0.700, 0.680 |
| 05 | F11 → projector | Left half (monitor 2) shows the warped quad, clean on black — no UI/handles |

## Gotcha (do not regress — see LESSON-TEST-004)

GLFW translates keys by **scancode**, not virtual-key. SendKeys and `keybd_event` with
`bScan=0` silently drop extended arrow keys (they map to `GLFW_KEY_UNKNOWN`). The harness
sends real scancodes via `MapVirtualKey` + `KEYEVENTF_SCANCODE` (+`EXTENDEDKEY` for arrows).

## TODO

- Add programmatic PASS/FAIL assertions (currently a human/agent reads the PNGs). Options:
  OCR the Properties values, or a `--smoke` app mode that dumps corner values + an output
  framebuffer hash to stdout for the harness to assert.
