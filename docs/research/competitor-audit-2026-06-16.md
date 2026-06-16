# Projection-Mapping Market Audit + Feature Roll-In Plan

**Date:** 2026-06-16
**Method:** 4 parallel web-research agents (pro tier / prosumer-free / node-&-code / AI-mobile-education),
each verifying pricing, platform, AI features, and status against primary sources in 2026.
Synthesis + replication grading done in main session (Opus). Companion to
[AI-preshow-mapping-2026-06-16.md](AI-preshow-mapping-2026-06-16.md).

**Strategic thesis:** QUADmapper is deliberately a *bare-bones* quad-warp mapper. The audit
asks: which competitor functionality must we match to be credible for **artists and students**,
how hard is each to replicate, and where do the AI supplements from the prior research turn
"bare-bones" into "invaluable"? The headline finding: **the affordable + easy + AI-assisted
niche is genuinely empty** — Lightform vacated it in 2022 and nobody refilled it.

---

## 1. The market gap (what the audit proves)

Three verified facts define the opening:

1. **Camera auto-warp is paywalled to the high end.** disguise OmniCal (server-locked,
   quote-only) and Scalable Display (installation tier) are the real deal. The *cheapest* real
   camera-assisted tool is **MadMapper's Space Scanner (~€44 add-on)** — but it needs a separate
   DSLR, uses projector-on structured light, and is semi-manual. *No consumer/phone tool does
   automatic camera auto-warp.* [madmapper.com/madmapper/features, disguise.one/products/omnical]

2. **Lightform — the one consumer AI auto-calibration product — is dead.** Hardware ended
   2022-02-07, cloud/services ended 2022-08 (acquired by Amazon). Its camera structured-light
   auto-scan (auto-masks + depth) was unmatched and **no survivor replaced it.**
   [lightform.com/blog/lightforms-sunset]

3. **Mobile "AI" mapping is vaporware.** Pro Mapper's "object-detection mapping" has no stated
   ARKit/LiDAR backing; SurfaBeam's "AI leveling" is contradicted by its own store listing
   ("does not access the camera"). HeavyM's "AI projection mapping 2026" is *generative effects +
   mask assist*, **not** camera auto-calibration. [apps.apple.com Pro Mapper, surfabeam.com,
   heavym.net/ai-projection-mapping-2026]

**Net:** free/cheap student tools are either dated (MapMap, VPT8) or hard (TouchDesigner,
openFrameworks); the easy modern ones cost money (HeavyM from ~$229, Resolume €799); and **none —
free or paid-under-$300 — offer real camera auto-warp or a 3D preview.** That is the wedge.

---

## 2. Competitor landscape (2026, verified)

| Product | Tier | Platform | 2026 price | Auto-cal / AI (shipped) | Status |
|---|---|---|---|---|---|
| **disguise** Designer/d3 | Top | Win + HW | $159/mo–$1.5k/yr + server | **OmniCal** structured-light auto-cal; "Ask AId3n" Gemini assistant; Volinga NeRF | Active |
| **Smode** | Top | Win only | €300–€3000/yr, sub-only | StreamDiffusion generative (Sep 2025); auto-cal outsourced to VIOSO | Active |
| **Resolume** Arena | Pro | Win/Mac | €799 perpetual, 50% edu | **None** — fully manual | Active |
| **MadMapper** | Pro | Win/Mac | €399 perpetual / €39mo, 30% edu | **Space Scanner** structured-light + DSLR auto-detect (not generative AI) | Active |
| **Millumin** V5 | Pro | **Mac only** | €399 perpetual; students €15/yr | Apple-Vision ML (person/bg detect); **no** projector auto-cal | Active |
| **Notch** | Pro | Win | $279/yr indie–$2.6k/yr | None shipped for auto-cal | Active |
| **TouchDesigner** | Node | Win/Mac | Free non-comm / $300 edu / $600 | **StreamDiffusionTD** real-time generative; mapping manual (KantanMapper/Stoner) | Active |
| **vvvv** gamma | Node | Win | Free non-comm | None first-party | Active |
| **HeavyM** | Prosumer | Win/Mac | ~$338–$678 lifetime / ~$33mo | **Generative AI effects** + mask assist (NOT camera auto-cal) | Active |
| **QLab** | Theatre | Mac only | ~$399 perpetual | None | Active |
| **Splash** (SAT) | OSS | **Linux/Mac** | Free GPLv3 | **Auto-calibrates** projectors (intrinsic/extrinsic/blend/color) | Active |
| **MapMap** | OSS | Win/Mac/Linux | Free GPL | None | Slow |
| **VPT 8** (HC Gilje) | OSS | Win/Mac | Free (Max) | None | Legacy (2018) |
| **Processing+Keystone** | Code | X-plat | Free | None | Stale |
| **openFrameworks** ofxPiMapper | Code | X-plat/Pi | Free | None | Active |
| **Map Club** | Web | Browser | Free | None (shaders only) | Active |
| **Lightform** | — | — | **DISCONTINUED 2022** | *was* consumer camera auto-scan | **Dead** |

*Closest competitor to "bare-bones quad mapper for artists/students":* **MadMapper** (cheapest
approachable perpetual) — but it still fronts a feature-dense pro UI at €399. The truly cheap
options are all either dated, coding-required, or platform-locked.

---

## 3. Functionality taxonomy — who has it, what we have, how hard to replicate

Replication difficulty for QUADmapper (C++17 / OpenGL / FFmpeg, Windows): 🟢 days · 🟡 ~1–2 wks ·
🔴 weeks–month+. "Have" reflects the current app (quad add, mesh subdivision, keyboard vertex
warp, image/video/procedural media, multi-output, perform mode, audio mixer).

### Core mapping
| Feature | Who has it | QUADmapper | Replicate |
|---|---|---|---|
| Quad / corner-pin warp | Everyone | ✅ Have | — |
| Bezier / mesh free-form warp | MadMapper, Millumin, TD | ✅ Have (mesh subdiv) | — |
| Feathered **masking** | MadMapper, HeavyM, Resolume, QLab | ❌ | 🟡 Stencil/alpha masks in GL |
| **Slicing** (1 source → many output regions) | Resolume, MadMapper, disguise | ⚠️ Partial (multi-output) | 🟡 |
| **Soft-edge blending** (projector overlap) | Resolume, MadMapper, Splash, disguise | ❌ | 🔴 Blend curves + gamma + overlap geometry |
| **3D object mapping** (map onto a model) | MadMapper, Smode, disguise, Splash | ❌ | 🔴 Model import + camera-calibration + UV |

### Media & I/O
| Feature | Who has it | QUADmapper | Replicate |
|---|---|---|---|
| Video / image playback | Everyone | ✅ Have (FFmpeg) | — |
| Procedural / GLSL shaders | MadMapper, TD, Smode | ⚠️ Procedural sources | 🟡 Live GLSL editor |
| Audio-reactive (FFT) | MadMapper, HeavyM, TD | ⚠️ Has audio module | 🟡 Wire FFT → params |
| **NDI** in/out | Resolume, MadMapper, QLab, disguise | ❌ | 🟡 NDI SDK (free) |
| **Spout** (Win) / Syphon (Mac) | MadMapper, Resolume, VPT, TD | ❌ | 🟡 Spout SDK (Win, easy) |
| Live camera input | MadMapper, VPT | ❌ | 🟢 (also needed for auto-warp) |

### Show control
| Feature | Who has it | QUADmapper | Replicate |
|---|---|---|---|
| Timeline / cue sequencing | HeavyM, QLab, Millumin | ❌ | 🔴 |
| MIDI / OSC / DMX / ArtNet | Resolume, TD, Smode, VPT | ❌ | 🟡 (OSC/MIDI first; DMX later) |
| Ableton Link sync | MadMapper, Resolume | ❌ | 🟡 |

### Calibration & AI — the differentiators
| Feature | Who has it | QUADmapper | Replicate |
|---|---|---|---|
| Structured-light auto-cal (gray code, projector-on + camera) | disguise, MadMapper, Splash | ❌ | 🟡 **OSS exists, MIT** — port kamino410 / punpongsanon graycode-procam |
| **Passive phone-photo auto-warp** ("Snap-to-Surface") | **NOBODY** (Lightform was closest, dead) | ❌ | 🟡 MVP (depth model + 4-tap homography) |
| **3D scene capture → projector-off preview** (phone GS) | NOBODY as a product | ❌ | 🔴 GS import + GL splat render |
| Generative AI content (depth-conditioned) | HeavyM, Smode, TD | ❌ | 🟡 Shell to local SD/ControlNet server |

---

## 4. Roll-in plan: bare-bones → invaluable for artists & students

Two layers. **Layer A** = cheap table-stakes that close the credibility gap with free competitors.
**Layer B** = the AI differentiators from the prior research that *no one else has* — the reason a
student picks QUADmapper over MapMap or a €399 MadMapper.

### Layer A — table-stakes (do first; mostly 🟢/🟡, high ratio of credibility per hour)
1. **Masking** 🟡 — universally expected; cheap in GL.
2. **Spout (Win) + NDI** 🟡 — lets QUADmapper sit in any artist's existing pipeline (feed from
   TouchDesigner/Resolume). Punching above weight for the effort.
3. **Live camera input** 🟢 — trivial on its own *and* the prerequisite for every auto-warp idea.
4. **Live GLSL shader source + audio-reactive wiring** 🟡 — the "creative" hook for students; we
   already have procedural + audio modules to build on.
5. **OSC/MIDI control** 🟡 — minimal show-control so it's usable in a real performance/class.

### Layer B — the differentiators (the wedge; ordered by impact-to-effort)
1. **⭐ "Snap-to-Surface" passive phone auto-warp** 🟡 — *the* unfilled niche. NOVELTY high
   (nobody ships passive phone auto-warp; patent-clean vs Christie's structured-light claim),
   IMPACT high (kills manual corner-drag — the core pain), exploits the phone link we already
   have. MVP: phone photo → off-the-shelf monocular depth (Depth Anything V2, ONNX) proposes the
   surface → user confirms 4 corners → warp pushes. "AI proposes, human confirms."
2. **Structured-light auto-warp (port OSS)** 🟡 — the *proven* fallback/complement to #1, and a
   credible "we have auto-cal too" story. MIT-licensed gray-code procam code exists to port; this
   is the conservative path if passive proves too noisy.
3. **⭐ Projector-Off 3D Preview (phone-splat digital twin)** 🔴 — the headline "wow." Capture the
   venue as a Gaussian splat with the phone, preview the whole show in 3D before the projector is
   plugged in. NOVELTY medium-high (GS+projection sim is published academically as GS-ProCams but
   shipped by nobody). Big build — later phase; shares the phone-capture rail with #1.
4. **Surface-aware generative content** 🟡 — add-on once a depth/splat capture exists; lowest
   novelty (ControlNet-depth is commoditized), so deprioritize.

### Positioning statement
> **QUADmapper = the free/cheap, keyboard-fast quad mapper that auto-warps from a phone photo —
> the Lightform niche nobody refilled, minus the hardware and the paywall.** Two-minute workflow
> for students; AI auto-warp + 3D preview that even €799 tools don't offer.

### Recommended build order
**Phase A (parity):** masking → live camera → Spout/NDI → GLSL+audio → OSC/MIDI.
**Phase B (wedge):** Snap-to-Surface MVP → (validate) → structured-light fallback → 3D preview →
generative.
Camera input is the hinge — it serves both the table-stakes (live source) and every Layer-B AI
feature, so it's the highest-leverage single item.

---

## 5. Honesty flags & what would change this
- **Auto-warp accuracy is unproven for us.** Passive phone warp is novel *because* it's hard;
  the 4-tap-assisted MVP de-risks it, but a pure no-input version may not be reliable. Build the
  MVP and *verify on a real projector* before claiming the niche.
- **Soft-edge blending and 3D-object mapping are 🔴** and are where the pro tools earn their price
  — not student table-stakes. Deliberately *out of scope* for the wedge; revisit only if demand.
- **Patent caution stands:** Christie US10,089,778 covers structured-light gray-code procam.
  Porting OSS gray-code code (#B2) needs a freedom-to-operate check; the passive path (#B1)
  sidesteps it.
- Prior-art facts are high-confidence (sourced); the "nobody ships passive phone auto-warp"
  novelty claim is an absence-of-evidence argument — a patent search hardens it before we commit.

**Sources:** see inline citations in §1–2 and the companion AI research doc. Full verified source
list (22 prior-art + ~24 competitor URLs) captured across the two research passes.
