# Current Status

**Last updated:** 2026-10-21 (moving to the owner's RTX 5090 test PC: read `000_start.md` there first)
**Phase / focus:** Release preparation. The AI features from ADR-053 are built. The owner tests them by hand next, then the installer (ADR-057, WIP on `feat/installer` at `9e5aacd`), then release.
**Dev:** all finished work is on `dev` and pushed. Full ctest 1730/1730 and sidecar pytest 89/89 at `57b833e`; later commits are docs, ADR-057 and owner files only.
**Before the AI test on the 5090:** move the sidecar to the PyTorch cu128 builds (`000_start.md` §4).

## What is done

- **Editor architecture (ADR-051):** panels, ActionRegistry, shortcuts and rebinding, and the perform lock. The GUI check of the lock and the Keyboard preferences is still pending by hand.
- **AI requests run off the main thread** (`ae8c4a0`): no more freeze. One request at a time. Request ceiling 120 s, and the sidecar warms its models at start.
- **ADR-053 accepted and built** (see its Amendment 1):
  - `tools/ai-sidecar/setup.ps1` builds an isolated venv (5.2 GB) and downloads SAM 2.1 small (184 MB), tiny (156 MB) and Depth Anything V2 Small (99 MB), SHA-256 checked.
  - Pick Surface with the AI Photo panel (SAM 2). Detect Surface on SAM 2 + Depth Anything V2 Small. Metric3D and Open3D are removed.
  - Every AI warp is one undo step.
  - Scan Output (Gray-code, session map). Pick and Detect map through the scan, then One-Flash, then photo space.
  - Place From Photo in the calibration view.
- **Nothing in the AI features has been checked by the owner on a real camera.**

## Owner's morning test (about 15 minutes, webcam aimed at the display or a projector)

Run `tools/ai-sidecar/setup.ps1` once first.
1. AI > Enable AI Assist. Wait for "sidecar connected".
2. Select the output to scan (else the first enabled one is used).
3. AI > Scan Output. The output goes fullscreen with stripes for about 15 s. Esc stops it.
4. The app leaves fullscreen and the status reads "Scan: map ready - the camera saw N% of the output (noise X px)". N should be high, X low.
5. Pick Surface...: take a photo in the AI Photo panel, click the surface, check the outline, press Apply. The status should say the corners went through the scan. Tab and arrows refine; Ctrl+Z undoes.
6. Calibration view: select a point, Place From Photo, click the same spot in the AI Photo panel. Ctrl+Z undoes it.
7. Detect Surface: the status begins "AI: surface found (confidence X) - ". Low confidence applies nothing.
8. Run the success metric once: add a quad, Tab and arrow warp, F11 to a projector.

Write down anything that looks wrong. Known risks to watch: the 250 ms camera settle time (TD-39), stale maps after a display mode change (TD-37), and misplaced corners on sliced or rotated outputs (TD-38).

## What is left before release

1. Fixes from the owner's test (QM-124).
2. Full ctest, once. Reconfigure and rebuild first (`cmake --preset debug`, then `cmake --build build/debug`), so a stale build does not pass.
3. Sidecar pytest (`tools/ai-sidecar`).
4. `/release`.
5. The `dev`→`master` PR. The prepared body is [docs/QM-061-pr-prep.md](../docs/QM-061-pr-prep.md); fill in the test counts and the hands-on result.

## Hardware-only items

- 🚩 **QM-061**: One-Flash and the AI features on a real projector, real surface. Runbook: [docs/QM-061-hardware-test-runbook.md](../docs/QM-061-hardware-test-runbook.md).
- TD-12 present threads, TD-26 audio device latency (hardware remainder).
- TD-37, TD-39: need a projector and a camera to measure.
- 3D-B2 (ChArUco camera calibration, stereo, ICP) is future work and needs its own ADR-053 amendment.

## Reference links

- Decisions: @docs/adr/0053-ai-surface-proposals-and-structured-light.md (Amendment 1) · @docs/adr/0035-ai-preshow-sidecar.md
- Tasks: @TASKS.md (QM-123, QM-124) · Debt: @TECHNICAL_DEBT.md (TD-37..TD-40) · Success metric: @CLAUDE.md#success-metric

<!-- Rewrite this whole file at /end. It is the restart package: the next session reads
THIS (plus the latest docs/sessions/ log), not the chat history. Keep it to one page. -->
