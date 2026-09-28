# Perform-mode stress / leak harness — specification

- **Status:** Planned (no ADR required — additive test tooling, no Change-Boundary surface).
- **Task:** QM-081
- **Date:** 2026-06-19
- **Source of idea:** MultiMon (`D:\AIprojects\MultiMon`) — `MultiMon.Stress` was its single most
  valuable engineering asset; it located a structural deadlock that 18 surface-level fixes missed.
  The lessons baked into this spec: LESSON-ARCH-004, LESSON-API-003, LESSON-BUG-005/006, LESSON-TEST-001.

## Why

QUADmapper's CLAUDE.md states the rule repeatedly: **"green tests ≠ done — 633 tests pass while the
real interaction could still be broken."** The missing gate is exactly the class of bug unit tests
cannot see: **resource leaks and wedges across repeated enter/exit of perform mode**, and **state
that silently resets when a per-show object is rebuilt** (the MultiMon re-perform audio-clock and
decoder-pool bugs are textbook examples). A headless harness that cycles EnterPerform → ExitPerform
N times while watching GL-object counts, memory, and frame progress closes that gate.

This is the highest-value, lowest-risk item from the MultiMon cross-pollination: it needs no new
dependency and no architecture change, and it de-risks the HAP work (ADR-044) once that lands.

## Shape

A **mode of `QUADmapper.exe`**, not a separate process — GL needs a real context + windows, and we
want to exercise the *actual* perform path, not a mock. Driven by CLI flags; bypasses the ImGui UI
and drives `OutputManager` / perform-mode entry directly.

```
QUADmapper.exe --stress --cycles=50 --outputs=2 --fullscreen \
               --frames-per-cycle=120 --pause-after=30 --capture=stress_caps
```

Output windows may be created **off-screen / hidden** (default) so the run is unattended; `--show`
makes them visible for eyeballing.

## Cycle definition (mirror the app's real cycle)

Per MultiMon LESSON-BUG-005, the harness cycle MUST model the app's real cycle or it is blind to
per-cycle-rebuild bugs. One cycle:

1. Ensure N `OutputWindow`s exist + a scene with one quad + an assigned source (image / video / HAP /
   procedural, per `--source`). **Created once, before the loop** — reused across cycles.
2. **EnterPerform** — show/fullscreen the outputs, start the clock/render.
3. Render `--frames-per-cycle` frames, advancing time (and decoder, if video).
4. **ExitPerform** — hide outputs, stop the clock.
5. Repeat — **without destroying the GL context, windows, or shared resources.** If QUADmapper
   currently *does* tear those down per cycle, that is itself the first finding (it is the MultiMon
   anti-pattern that caused per-cycle native churn).

## Detection layers

### 1. GL-object leak (primary gate)
OpenGL has no global live-object query like D3D11's debug layer, but **every GL object in QUADmapper
is RAII-wrapped** (rendering rule). Add a `static std::atomic<int>` live-count per wrapper type
(`Texture`, `Framebuffer`, `VertexArray`, `VertexBuffer`, `Shader`/`ShaderProgram`, `Mesh`),
incremented in the ctor / `glGen*` and decremented in the dtor / `glDelete*`. Snapshot the counts
**after a warm-up cycle**, then assert **zero growth** across the remaining cycles. This is the
portable equivalent of MultiMon's live-object diff.

### 2. Working-set backstop
Per MultiMon LESSON-API-003, a flat object count is **blind** to some leaks (e.g. an object kept
alive off the wrapper graph, or driver-side allocations). Track process working set
(`GetProcessMemoryInfo`) with a **post-warm-up baseline + bound**; a clean run stays flat. Where
available, also log GPU memory via `GL_NVX_gpu_memory_info` / `GL_ATI_meminfo`.

### 3. Wedge watchdog
If a cycle does not complete its `--frames-per-cycle` within a timeout, log the offending cycle and
abort loudly (no infinite hang). This is the analog of MultiMon's present-rate watchdog that
localized its pause-freeze bug fast.

### 4. Visual gate (optional, `--capture`)
Per MultiMon LESSON-TEST-001, a frame *count* proves nothing about visible output — a black clear
counts as a frame. Read back an output FBO → PNG via the existing dependency-free **PngWriter**
([quadmapper-ffmpeg-build] memory) on the last frame of selected cycles, and assert **non-black /
expected checksum**. Ground truth, independent of window occlusion.

## Targeted scenario flags (each reproduces a known MultiMon bug class)

| Flag | Reproduces |
|---|---|
| `--pause-after=K` | Pause mid-cycle then resume — does our video decode thread + ring buffer survive a pause/resume? (MultiMon LESSON-BUG-006: held decoder-pool textures froze outputs on resume.) |
| re-perform (implicit, cycles ≥ 2) | Enter → exit → enter: assert no time jump and no audio-clock / master-volume reset on the second perform (MultiMon LESSON-BUG-005 / BUG-007). |
| `--source=hap` | Once ADR-044 lands: HAP decode + compressed upload across cycles, capability-fallback path exercised. |
| `--outputs=N --fullscreen` | Multi-output present + fullscreen toggle churn. |

## Pass criteria

- `cycles/cycles` complete; zero wedge; clean exit.
- RAII live-object counts **flat (±0)** from the post-warm-up baseline.
- Working set within the calibrated bound.
- `--capture` frames non-black / expected.

## Integration

- **`ctest` "stress-smoke"**: a low cycle count (e.g. `--cycles=5 --outputs=1`) wired as a CI test so
  gross leaks fail the build cheaply.
- **Release gate**: the full `--cycles=50 --outputs=2 --fullscreen` run is a manual / `/release`-time
  gate (MultiMon ran 50 cycles as its ship gate), exercised on each target GPU class where possible.

## Phasing

- **P1** — RAII live-count instrumentation + the headless cycle loop + the count gate. (Smallest
  shippable slice; already catches the most common leak class.)
- **P2** — working-set backstop + wedge watchdog + `ctest` stress-smoke wiring.
- **P3** — `--capture` visual gate (reuse PngWriter) + `--pause-after` + re-perform assertions +
  `--source=hap` once ADR-044 lands.

## H0 — implementation design (decided, Opus + high)

Grounded in the actual code, not the abstract spec:

- **Entry point.** `Application::initialize(int argc, char* argv[])` currently ignores its args
  ([Application.cpp:294](../src/app/Application.cpp#L294)). Parse `--stress` + sub-flags there into a
  `std::optional<StressConfig> m_stressConfig`. Flags: `--cycles=N`, `--outputs=N`,
  `--frames-per-cycle=N`, `--pause-after=K`, `--capture=DIR`, `--source=image|video|hap|procedural`,
  `--hidden` (default true once P2 lands). No env var — CLI is cleaner than the `QM_AI_SELFTEST`
  precedent, though that precedent confirms the headless-driver shape is already accepted here.
- **Run branch.** `Application::run()` ([Application.cpp:588](../src/app/Application.cpp#L588)) checks
  `if (m_stressConfig) return runStress(*m_stressConfig);` *before* the interactive `while` loop. The
  harness is a sibling run path, not a hijack of the UI loop.
- **`runStress(cfg)`** drives the **real** perform path so the test is meaningful:
  1. Ensure `cfg.outputs` outputs exist (`OutputManager::createOutput`, assign monitor, enable) + a
     scene with one quad + an assigned source of `cfg.source`. **Built once, before the loop.**
  2. **Warm-up cycle** (enter → N frames → exit), then snapshot the baseline counters + working set.
  3. Loop `cfg.cycles`: `enterPerformMode()` → render `frames_per_cycle` frames
     (`renderToOutputs` / `renderToProjectorOutputs`, advancing time; optional pause at
     `--pause-after`) → `exitPerformMode()` → snapshot + diff against baseline.
  4. Print a per-type object-count table + working-set delta + PASS/FAIL; return non-zero on FAIL.
- **Leak counters (H1).** A `core/GlObjectStats` facility: one `std::atomic<long>` live-count per GL
  wrapper type, incremented when a GL **name is generated** and decremented when it is **deleted** —
  tied to *owning a live GL name*, NOT to object lifetime. Move ctor/assignment transfer the name
  (moved-from handle set to 0; dtor guards on `handle != 0`) so a move must not double-count. Wrap
  `Texture`, `Framebuffer`, `VertexArray`, `VertexBuffer`, `ShaderProgram`/`Shader`, `Mesh`.
- **Windows.** P1 lets perform mode show real fullscreen windows (most faithful). `--hidden` (P2)
  keeps them off-screen for unattended/CI runs; the CI stress-smoke uses a tiny cycle count.

## Acceptance criteria

1. `QUADmapper.exe --stress --cycles=50 --outputs=2 --fullscreen` runs unattended to completion and
   prints a per-type object-count table + working-set delta + pass/fail.
2. A deliberately introduced leak (e.g. a `Texture` not released on ExitPerform) is **caught** by the
   count gate — i.e. the gate is proven to fail when it should.
3. `--capture` writes a non-black PNG per sampled cycle.
4. The stress-smoke `ctest` passes in CI; the harness adds no runtime cost to the normal app path
   (instrumentation is a couple of atomics).
