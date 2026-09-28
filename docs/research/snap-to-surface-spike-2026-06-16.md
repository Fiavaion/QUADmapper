# Snap-to-Surface Feasibility Spike — Findings (GO, with a reframed feature)

**Date:** 2026-06-16
**Goal:** Before committing a roadmap to the "Snap-to-Surface" wedge (feature #B1 in the
[competitor audit](competitor-audit-2026-06-16.md)), prove or kill the load-bearing assumption:
*can an off-the-shelf monocular depth model, run locally on this Windows box, turn a single
photo of a projection surface into a usable surface estimate in pre-show time?*

**Verdict: GO — but the spike reshaped the feature.** Local inference and surface recovery are
real and fast. Two limits surfaced that *redefine* the MVP: it must be **human-in-the-loop**
(not fully automatic), and the truly novel auto-warp is a **"one-flash"** capture, not a
projector-off passive one. Both are still buildable and still unserved by competitors.

---

## What was actually run (reproducible)

- **Box:** RTX 3080 Laptop (8GB), torch 2.7.1+cu128, CUDA live. Models already on disk
  (Depth-Anything-V2-Large 1.28GB; Metric3D 143MB) — **zero downloads**.
- **Inputs:** 3 controlled photoreal test scenes generated locally (JuggernautXL) — a paneled
  facade at a steep oblique angle, an art gallery (multi-plane), and a theatre with a real
  projection screen + foreground seats. (Proxy for phone photos; I can't take a real one in this
  environment, so I used controlled, relevant, license-clean inputs.)
- **Pipeline:** image → DA-V2 depth → back-project to point cloud (assumed 62° FOV, no EXIF
  intrinsics) → RANSAC dominant-plane fit → planarity residual + inlier mask.
- **Artifacts:** `snap-spike-evidence/` (3 inputs, 3 plane overlays, 1 depth map, the script).

## Quantitative result

| Scene | Inference | Plane planarity (RMS) | Dominant plane landed on… |
|---|---|---|---|
| Facade (oblique) | 0.6s* | **0.45%** of depth-span | far/flatter half of the wall ✅ surface, ⚠️ partial |
| Theatre screen | 0.6s | **0.37%** | **the projection screen** ✅✅ exactly right |
| Gallery | 0.7s | **0.51%** | **the floor**, not the back wall ❌ wrong surface |

\* steady-state; first inference 1.4s incl. CUDA autotune. Model load 6s, one-time.
Plane-fit took 1.5–10s only because the spike's RANSAC is naive pure-Python — Open3D
`segment_plane` does this in **milliseconds**; not a real cost.

**Read:** depth quality is excellent and planar surfaces fit *tightly* (sub-0.5% RMS). Inference
is trivially within a pre-show budget. The capture half of the feature is **de-risked.**

## The two limits the visuals exposed (this is the value of the spike)

1. **Affine ambiguity (relative depth).** DA-V2 `vitl` is *relative* depth — affine-ambiguous
   (unknown global scale + shift). A real plane at a steep angle **bows** in inverse-depth space,
   so a tight threshold captured only the far half of the facade. → **Fix:** use **Metric3D**
   (already on disk) for metric depth, or fit in a homography-aware space. Solvable, known.

2. **"Largest plane" ≠ "intended surface."** In the gallery the floor is the biggest coherent
   plane, so RANSAC picked it over the back wall. Fully-automatic, zero-input surface selection
   is therefore **unreliable in cluttered scenes.** → **Fix:** the human-in-the-loop step (user
   taps/confirms the target surface) is **load-bearing, not optional**. Optionally add semantic
   segmentation (wall vs floor) to *propose* better. This *confirms* the "AI proposes, human
   confirms" MVP design from the audit — the spike turned that from a hunch into a requirement.

> Net on capture: when the surface is unambiguous (the screen), it auto-nails the target. When
> it's cluttered, it needs one human tap. Either way the user does **far** less than dragging
> four corners by eye.

## The deeper geometric finding (reasoned, not coded — and it reframes the feature)

The spike recovers the surface **in the phone's frame**. But warping the *projector* output
needs the **projector→surface** correspondence, and a passive photo (projector off) does **not**
give projector pose. This is exactly why every competitor is "projector-ON" (structured light
*observes* where projector pixels land). So the pure "projector-off passive auto-warp" framing
has a hard geometric limit. Three viable modes remain — and they're better-defined than #B1 was:

1. **⭐ One-Flash Auto-Warp (projector ON, single photo).** Projector shows one test rectangle;
   the phone photo sees the surface **and** that rectangle together → solve the homography
   mapping projector-quad → surface directly. **No projector pose needed** (observed). One photo,
   **not** a gray-code sequence — materially faster/lighter than MadMapper Space Scanner or
   OmniCal, and no DSLR/mount. *This is the strongest, most defensible near-term feature.*
2. **⭐ Audience-Perspective (Anamorphic) Warp.** Warp content so it looks geometrically correct
   from the **phone's viewpoint = the audience sweet-spot**. Uses the surface capture directly;
   genuinely novel and artful (trap-art / anamorphosis) — a creative hook for artists/students.
3. **Surface-Snap Assist (what was tested).** Depth/boundary auto-proposes the quad corners on
   the detected surface; user confirms. Cuts manual corner-hunting even with projector off.

## Go/No-Go

- **GO on the capability.** Local, fast, accurate surface recovery is proven on this exact
  hardware with already-present models. No new dependency risk for the AI half (Python sidecar:
  torch + Open3D + OpenCV; the C++ app shells out / talks over the existing local link).
- **Build the MVP as One-Flash Auto-Warp (mode 1) + human-confirm (limit 2),** using Metric3D
  for the fit (limit 1). Keep Anamorphic (mode 2) as the artist-facing follow-up.
- **The homography math itself is not a risk** (textbook `cv2.findHomography`); I did not spend
  tokens re-proving it.

## Open items before roadmap
1. **Real device test.** Validate on an actual phone photo of an actual projector + surface
   (the one thing this environment can't do). The proxy images are realistic but synthetic.
2. **Metric3D plane-fit re-run** to confirm it fixes the facade bow (quick; model is on disk).
3. **Projector intrinsics** — One-Flash needs a rough projector model; confirm we can get it from
   throw-ratio metadata or a one-time calibration, else it folds into the test-pattern solve.
4. **C++ ↔ Python boundary** — decide sidecar process vs embedded (pybind). Sidecar is lower-risk
   for an optional pre-show feature; keeps the 60-FPS render path untouched.

**Evidence:** `docs/research/snap-spike-evidence/` — see `test_screen_plane.png` (the win) and
`test_wall_plane.png` (the floor-not-wall limit). Script: `depth_spike.py`.
