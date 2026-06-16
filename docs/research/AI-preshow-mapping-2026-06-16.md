# AI in the Pre-Show Mapping Stage — Prior-Art & Novelty Research

**Date:** 2026-06-16
**Method:** Deep-research workflow — 5 search angles, 22 sources fetched, 103 claims
extracted, top 25 adversarially verified (3-vote, ≥2 to confirm). 25/25 confirmed, 0 killed.
**Synthesis:** main session (Opus), after the workflow's auto-synthesizer failed its
structured output. Confidence flags are mine, grounded in the verified claim set below.

**Scope locked with the user:** pre-show **setup only** (latency-tolerant, heavy models OK,
no live tracking); sensing = RGB camera / depth-LiDAR / **phone** (reuses the existing mobile
remote link); three capabilities — auto-calibration, Gaussian-splat reconstruction, generative
content.

> **Confidence caveat (read first):** the *prior-art* findings are high-confidence (verified
> against primary sources — papers, product docs, a US patent). The *novelty* findings are
> **medium** confidence: they are an absence-of-evidence argument ("every verified method is
> projector-ON, therefore passive/projector-OFF is unexplored"), not proof no one has shipped
> it. Two things would harden them: (1) a proper patent search around the phone-photo-warp
> idea; (2) confirming Lightform's discontinued LFC pipeline (Lightform folded — its IP is the
> closest commercial precedent). Treat the shortlist as *directional bets*, not settled fact.

---

## Capability 1 — Auto-calibration / auto-warp

### Already done / commoditized (high confidence)

| Who | What | Key trait (verified) |
|---|---|---|
| **disguise OmniCal** | Commercial camera-based projector calibration | Structured-light patterns; reconstructs the physical surface; **requires proprietary camera hardware**; high-end/touring tier |
| **MadMapper Space Scanner** | Prosumer auto-calibration | Structured light (**gray code**); **uses the projector itself as the light source**; needs a camera |
| **kamino410/procam-calibration** (OSS) | Projector-camera calibration tool | Gray-code structured light; establishes projector↔camera pixel correspondence |
| **punpongsanon/graycode-procam-calibration** (OSS, MIT) | Procam calibration | Dense gray-code correspondence map |
| **DeProCams** (arXiv 2306.06595) | End-to-end trainable procam model | Models projector response; **waives extra optical devices**; projected texture fully differentiable |
| **Christie Digital — US Patent 10,089,778** | Patent | Claims **structured-light gray-code** procam; embeds projector as a high-res sensor |
| **HeavyM** | Prosumer mapping | Warping is **manual**; "photo as a reference" is a backdrop, **not** auto-calibration |
| MadMapper / Resolume / TouchDesigner-KantanMapper | Core warp | Manual corner/mesh dragging (the status quo QUADmapper matches) |

**The pattern across every single one:** calibration is **projector-ON and active** — it
projects a gray-code/structured-light sequence and films it. The high-end ones also demand a
mounted/proprietary camera. The OSS ones demand a calibrated rig.

### Genuinely unexplored (medium confidence)

- **Passive, marker-less, single-photo phone warp.** Snap one photo of the target surface with
  the phone you're already holding — *no projected pattern sequence, no printed ChArUco marker,
  no rigid camera mount* — and have the app infer the surface and compute the warp. No verified
  product or paper does this; all prior art is active/projector-on.
- **Patent-clean bonus:** staying *passive* also routes around the Christie gray-code patent
  family (US10,089,778), which is a structured-light claim. Strategic, not just novel.

---

## Capability 2 — Gaussian splatting / 3D reconstruction

### Already done (high confidence) — this downgrades raw "GS for projection" novelty

| Source | Finding (verified) |
|---|---|
| **GS-ProCams** (arXiv 2412.11762) | "**First Gaussian-Splatting-based framework for projector-camera systems**"; enables **view-agnostic simulation of projection** (confirmed 2-1 — narrow) |
| **DeProCams** (arXiv 2306.06595) | Differentiable procam; projected texture differentiable w.r.t. the scene |
| **arXiv 2503.12174** | Further GS/radiance-field projection-simulation work |
| **Polycam / Scaniverse** | Phone → Gaussian-splat capture is commoditized and consumer-grade |
| **Apple RoomPlan** | Phone-LiDAR room scan → parametric geometry (consumer) |

So **GS + projection simulation already exists in academia** (GS-ProCams, late 2024 / 2025).
Phone→splat capture is a solved consumer problem. Raw "use Gaussian splatting for projection"
is therefore **NOT novel** — it's published. Be honest about that.

### Genuinely unexplored (medium confidence)

- **Projector-OFF "digital twin" preview.** Capture the venue with your phone as a splat, import
  it, and **design + preview the entire show inside that 3D capture before the projector is ever
  plugged in or aimed**. Every verified GS/procam method is *projector-ON* (it needs the live
  procam loop to fit the model). A *projector-off, phone-captured, content-authoring* preview is
  a different framing and is unexplored as a **product**. (Academic GS-ProCams simulates
  projection, but inside the captured-with-projector loop — not as a desk-side pre-viz tool.)

---

## Capability 3 — Generative / assistive content

### Already done (high confidence)

- **Depth-conditioned generation** (ControlNet + Stable Diffusion) for mapping content —
  documented DIY workflow since 2023 (aboutme.be).
- **TouchDesigner + StreamDiffusion** — real-time diffusion in a mapping tool, established.
- **Lightform Creator** — surface-aware content tooling (company now defunct).
- **HeavyM "AI projection mapping"** marketing — generative content is being productized.

The **building blocks are commoditized.** "AI makes the visuals" is not novel.

### Possibly novel (LOW confidence — absence of evidence)

- **Zero of the 25 verified claims covered generative content fitted to *reconstructed 3D
  geometry*.** The only fresh angle is content conditioned on the **splat surface** (true
  geometry) rather than a flat depth map — but this scored "cleanest novelty" mostly because
  nobody *researched* it, i.e. weakest evidence. Lowest-confidence of the three. Treat as a
  later add-on, not a wedge.

---

## Ranked shortlist of novel pre-show AI features

Scoring: **Novelty** (absent from commercial + OSS prior art) · **Feasibility** (solo dev,
C++/OpenGL/Windows, off-the-shelf models) · **Impact** (cuts setup time / unlocks capability).

### #1 — "Snap-to-Surface": passive phone-photo auto-warp  ⭐ recommended wedge
- **Novelty: HIGH** — all prior auto-calibration is projector-on/structured-light; passive
  single-photo marker-less warp is unseen, and it sidesteps the Christie gray-code patent.
- **Feasibility: MEDIUM** (MVP **HIGH**). Full vision (zero user input) is hard — unknown
  projector pose. **MVP that's tractable now:** phone photo → off-the-shelf monocular depth
  (Depth Anything V2 / MiDaS, ONNX Runtime) + plane segmentation to *propose* the surface, user
  confirms/taps 4 corners in the photo → solve homography → push the warp to the quad. "AI
  proposes, human confirms" — ships fast, degrades gracefully.
- **Impact: HIGH** — directly attacks manual corner-dragging, the app's core friction, and
  exploits the phone link QUADmapper already has.

### #2 — "Projector-Off Preview" (phone-splat digital twin)  ⭐ visionary differentiator
- **Novelty: MEDIUM-HIGH** — GS+projection sim is published (GS-ProCams), but a projector-off,
  phone-captured, desk-side **pre-viz** product is unexplored.
- **Feasibility: MEDIUM-LOW** — needs phone-GS import (Polycam/Scaniverse export, or a GS lib),
  a GL splat renderer in-app, and projecting quad content onto the splat virtually. Biggest lift:
  GS rendering integration + getting projector pose into the splat frame.
- **Impact: HIGH** — "design the whole show at your desk, no venue access, no projector" is a
  genuinely new capability for touring VJs / installation artists.

### #3 — Surface-aware generative content
- **Novelty: LOW-MEDIUM** (building blocks commoditized; only the splat-geometry conditioning is
  fresh — and it's the lowest-confidence finding).
- **Feasibility: MEDIUM** — ControlNet-depth pipeline is off-the-shelf, but wiring local SD into
  a C++ app is friction (shell out to a local server). **Impact: MEDIUM** — nice-to-have, not the
  core pain.

### Recommendation
Lead with **#1 (Snap-to-Surface)** — best impact-to-effort, hits the core pain, patent-clean,
and the phone is already wired in. Hold **#2 (Projector-Off Preview)** as the headline
differentiator for a later phase (it's the "wow," but a real build). **#3** is an add-on after a
splat pipeline exists. #1 and #2 **share infrastructure** (phone capture + depth/geometry), so #1
naturally paves the road to #2.

---

## Sources (22 fetched; primary unless noted)

**Auto-calibration (academic):** arXiv 2412.11762 (GS-ProCams), 2003.03040, 2102.11395,
1803.09058 · github.com/kamino410/procam-calibration · github.com/punpongsanon/graycode-procam-calibration
**Auto-calibration (commercial):** disguise OmniCal (help.disguise.one, disguise.one) ·
madmapper.com · heavym.net · USPTO US 10,089,778 (Christie) · studio-z.ca comparison (blog)
**Gaussian splatting / projection:** arXiv 2306.06595 (DeProCams) · arXiv 2503.12174 · poly.cam
**Generative content:** aboutme.be (blog) · heavym.net/ai-projection-mapping-2026 (blog) ·
lightform.com/creator · interactiveimmersive.io StreamDiffusion+TD (blog)
**Phone-as-capture:** ieee 10108826 · Simform RoomPlan (blog) · lightform.com projected-AR (blog)

## What would change this assessment
1. A patent search around "passive single-image projector warp" before committing to #1.
2. Confirming the discontinued **Lightform LFC** pipeline scope (closest commercial precedent;
   company defunct, IP status unknown).
3. The narrow 2-1 vote on GS-ProCams "view-agnostic simulation" — worth re-reading the paper
   directly before leaning on #2's novelty claim.
