# UI Smoke Harness

`drive_smoke.ps1` is the verification gate for the [ADR-031](../../docs/adr/0031-keep-vs-rebuild.md)
refactor: it drives the **real** app to exercise the success-metric workflow that unit
tests can't reach, then captures screenshots for visual + numeric verification.

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

## Verify

Read the PNGs in `out/` (gitignored). The Properties panel shows the live corner values —
check them against the expected math. Known-good baseline (default quad 0.3–0.7):

| Step | Action | Expected corner |
|------|--------|-----------------|
| 02 | corner0: Shift+Right×20, Shift+Down×10 | Top Left 0.500, 0.400 |
| 03 | corner1: Shift+Left×15 | Top Right 0.550, 0.300 |
| 04 | corner2: plain Up×20 | Bottom Right 0.700, 0.680 |

## Gotcha (do not regress — see LESSON-TEST-004)

GLFW translates keys by **scancode**, not virtual-key. SendKeys and `keybd_event` with
`bScan=0` silently drop extended arrow keys (they map to `GLFW_KEY_UNKNOWN`). The harness
sends real scancodes via `MapVirtualKey` + `KEYEVENTF_SCANCODE` (+`EXTENDEDKEY` for arrows).

## TODO

- Extend to assert **projector output** once TD-01 wires `OutputManager::renderToOutputs()`.
  Needs an output-creation path the harness can drive headlessly (CLI flag or a "send selected
  to output N" key), then capture the second monitor and verify the warped quad appears.
