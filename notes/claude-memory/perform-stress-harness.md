---
name: perform-stress-harness
description: QUADmapper has a headless --stress perform-cycle leak/wedge harness (QM-081); how to run it and what it proved.
metadata: 
  node_type: memory
  type: project
  originSessionId: 7af9a884-6117-4eb5-97c2-2dbff43dc4ae
---

QUADmapper has a headless perform-mode stress/leak harness (QM-081, Session 21), cross-pollinated
from the MultiMon project. Run it via `QUADmapper.exe --stress` with flags:
`--cycles=N --outputs=N --frames-per-cycle=N [--fullscreen] [--hidden] [--source=procedural]
[--ws-bound-mb=N] [--cycle-timeout-ms=N]`. It cycles `enterPerformMode`/`exitPerformMode`,
gating on three things: GL-object live-count flat (the `core/GlObjectStats` atomics in the GL
wrappers + Renderer), process working-set growth within a bound (`GetProcessMemoryInfo`/psapi),
and a per-cycle wedge timeout. `--hidden` runs it without grabbing the desktop; a `stress_smoke`
ctest (`--cycles=5 ... --hidden`) is wired as a CI gate.

**Why:** closes the "633 tests green but the real perform interaction could still leak/wedge" gap
CLAUDE.md flags repeatedly.

**How to apply:** run the full `--cycles=50 --outputs=2 --fullscreen` form as a manual/`/release`
gate before shipping; the `stress_smoke` ctest catches gross leaks every build.

**Proven:** the app is leak-clean across perform cycles (counts flat, ~8 MB WS delta / 5 cycles).
Key design fact it confirmed — per-output `Renderer`s are created in `OutputManager::createOutput()`
and are **persistent**, NOT rebuilt per perform cycle (QUADmapper already follows MultiMon's
build-once-reuse discipline). See `docs/perform-stress-harness-spec.md`. Related: [[build-stale-relink]].
