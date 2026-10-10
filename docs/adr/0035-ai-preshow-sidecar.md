# ADR-035: AI pre-show assist via an out-of-process Python sidecar

**Date:** 2026-06-16
**Status:** Accepted (2026-06-16, Jones)
**Deciders:** Jones

## Context

The market audit ([competitor-audit-2026-06-16.md](../research/competitor-audit-2026-06-16.md))
and the AI prior-art research ([AI-preshow-mapping-2026-06-16.md](../research/AI-preshow-mapping-2026-06-16.md))
identified an unserved niche: an affordable mapper that auto-warps a quad onto a real surface from
a phone photo. The feasibility spike
([snap-to-surface-spike-2026-06-16.md](../research/snap-to-surface-spike-2026-06-16.md))
**verified on this exact hardware** that off-the-shelf monocular depth (Depth-Anything-V2 /
Metric3D) recovers a clean, plane-fittable surface from a single photo in ~0.6s, and that the
right MVP is **One-Flash Auto-Warp + human-confirm**.

That capability lives entirely in the **Python CV/ML ecosystem** (torch, Open3D, OpenCV,
Metric3D weights). QUADmapper is **C++17 / OpenGL / FFmpeg**, with a hard 60-FPS render path and a
no-touch build/dependency Change Boundary. The decision that is hard to reverse: **how does an AI
capability enter this app without destabilising the render loop, the build, or the `.qmap`
format?** Once an integration boundary is chosen and built against, moving it is expensive.

Two hard constraints frame it: (1) the feature is **pre-show only** — latency-tolerant
(multi-second is fine), never on the 60-FPS path; (2) it must **degrade gracefully** — the app is
fully usable with the AI component absent or failed (it is an assist, never a dependency of the
success metric).

## Decision

Integrate AI as an **optional, out-of-process Python sidecar** that the C++ app talks to over
**local IPC** (localhost HTTP + JSON, loopback-only), not by embedding Python in-process.

1. **Process boundary.** A standalone Python process (`tools/ai-sidecar/`) owns all ML/CV. The
   app launches it on demand (or attaches to a running instance) and health-checks it. If it is
   absent, unreachable, or errors, the AI UI affordances disable cleanly with a notice — **no
   effect on any existing workflow.**
2. **Contract, not coupling.** A small, versioned JSON request/response API:
   `POST /surface {image_path|frame} → {corners[4], plane, confidence}` and later
   `POST /oneflash {surface_img, pattern_img} → {homography, corners[4]}`. The app sends a
   captured frame and a request; the sidecar returns geometry. **The app never imports Python.**
3. **Result maps onto existing geometry.** Outputs are quad **corner offsets / mesh offsets** —
   the data `QuadGeometry` already holds and `.qmap` already serialises. **No `.qmap` schema break**
   (additive fields at most, under the ADR-032 versioning policy). The warp the AI produces is
   indistinguishable on disk from a hand-warp.
4. **Sidecar is not in the core build.** It ships as an **opt-in component** (its own
   `requirements.txt` / packaged runtime), never linked into `QUADmapper.exe`. The C++ build,
   vcpkg manifest, and CMake presets are **untouched** — staying inside the Change Boundary.
5. **GPU optional.** CUDA used when present (spike: ~0.6s on the RTX 3080); CPU fallback is slower
   but acceptable pre-show. Model weights (~150MB Metric3D) download/install with the component.

## Consequences

- ✅ **Render path & build untouched.** Zero risk to the 60-FPS loop or the success metric; no new
  C++/vcpkg dependency; Change Boundary respected (the sidecar is a separate, optional artifact).
- ✅ **Right tool for the job.** Reuses the exact Python stack the spike proved (Metric3D, Open3D,
  OpenCV) instead of reimplementing it in C++.
- ✅ **Graceful degradation by construction.** AI absent → app behaves exactly as today.
- ✅ **Format-safe.** Results are ordinary geometry; no breaking `.qmap` change.
- ✅ **Independently testable.** The sidecar API can be unit-tested in Python and contract-tested
  from the app without a GPU in CI.
- ❌ **A second runtime to ship and version.** Packaging Python + weights for end users is real
  work (installer story, version skew between app and sidecar API). Mitigated by the versioned
  API and treating it as an optional download.
- ❌ **IPC surface area.** Localhost HTTP must be loopback-bound and the spawned process lifecycle
  managed (orphan cleanup). Small, but real.
- ❌ **Not in-process** — a few hundred ms of IPC + image handoff overhead. Irrelevant pre-show.

## Alternatives considered

1. **Embed Python via pybind11 (in-process).** Rejected — couples a Python interpreter + torch
   into `QUADmapper.exe`, drags GIL/packaging/ABI concerns into the core build, and violates the
   build/dependency Change Boundary. All cost, no benefit for a latency-tolerant pre-show feature.
2. **Reimplement in C++ (ONNX Runtime + custom plane-fit).** Rejected — re-derives the proven
   Python pipeline (loses Metric3D + Open3D), multiplies build/dependency surface (ONNX Runtime,
   OpenCV-C++), and is far slower to ship. Revisit only if a *real-time* (on-path) AI feature is
   ever needed — a different ADR.
3. **Cloud inference API.** Rejected — live events need offline operation; adds latency, per-use
   cost, and a privacy problem (uploading the venue). The whole pitch is "with the phone you
   already hold," not "with a server you rent."
4. **stdio/pipe IPC instead of localhost HTTP.** Reasonable; deferred as an implementation detail
   of decision #2. HTTP+JSON chosen first for trivial debuggability (curl) and language-agnostic
   testing; can swap to stdio without changing the contract.

<!-- Accepted 2026-06-16. Introduces an optional Python runtime + model weights as a new
(out-of-build) dependency; the sidecar is never linked into QUADmapper.exe and the C++ build /
vcpkg manifest / CMake presets stay untouched. Roadmap: docs/research/ai-preshow-roadmap-2026-06-16.md
(QM-055..062). Implementation starts at QM-055 (live camera input). -->

**Note (2026-10-10):** [ADR-053](0053-ai-surface-proposals-and-structured-light.md) replaced Metric3D and Open3D with SAM 2 and Depth Anything V2 Small. The Metric3D checkpoint's licence is unconfirmed. Mentions of Metric3D and Open3D above describe the design as accepted in June 2026, not the sidecar as it is now.
