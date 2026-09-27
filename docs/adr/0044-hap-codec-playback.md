# ADR-044 — HAP / HapQ codec playback

- **Status:** **Accepted** (2026-06-19, approved as specified — vendored Snappy + FFmpeg-demux +
  capability-gated BCn upload). Cross-pollinated from the MultiMon project.
- **Date:** 2026-06-19
- **Task:** QM-080
- **Relates:** ADR-040 (shared media sources + per-quad crop), rendering rule (GL 3.3 floor),
  [test-media-location] memory (`D:\testing Videos\`)
- **Source of idea:** MultiMon (`D:\AIprojects\MultiMon`) shipped a from-scratch HAP decoder;
  its `notes/lessons.md` LESSON-ARCH-005 / LESSON-PERF-001 / LESSON-ARCH-006 are the guardrails below.

## Context

HAP is the de-facto codec for VJs, installation artists, and projection mappers — Resolume,
MadMapper, and TouchDesigner all support it. QUADmapper's target users (per CLAUDE.md: live-event
techs, VJs, installation artists) expect it, and we have **none** today (no hits for hap/snappy/
ycocg/BCn anywhere in `src/`). Today every clip goes through FFmpeg's full H.264/HEVC decode to
RGB(A) `FrameData` and a plain `glTexSubImage2D` upload ([SourceManager.cpp:296](../../src/source/SourceManager.cpp#L296)).

HAP's advantage is structural, not incremental: a HAP frame is **already a GPU-compressed BCn
texture**. Decoding is pure CPU (demux → optional Snappy decompress → concatenate chunks → BCn
bytes); the GPU receives compressed data and samples it directly. That is ~6–8× less PCIe upload
per frame and trivial GPU cost, so a machine that stutters on three 4K H.264 layers can run many
simultaneous 4K HAP layers — exactly the multi-output / multi-surface workload QUADmapper targets.

MultiMon's **LESSON-ARCH-005** is the key de-risking insight, and it transfers to our pipeline:

> HAP is CPU-decode + GPU-upload. It slots into the existing pipeline as a **texture-format +
> shader variant**, with zero new device/render state. No parallel renderer.

For us that means: HAP reuses the existing background-decode-thread → stage → main-thread-upload
model and the existing warp/render path. The only new pieces are a compressed-bytes staging buffer,
a `glCompressedTexSubImage2D` upload, and (for HapQ only) a YCoCg→RGB fragment-shader variant.

## HAP variant / format map

| HAP variant | Stored as | GL internal format | Shader |
|---|---|---|---|
| Hap | BC1 (DXT1, RGB) | `GL_COMPRESSED_RGB_S3TC_DXT1_EXT` | passthrough |
| Hap Alpha | BC3 (DXT5, RGBA) | `GL_COMPRESSED_RGBA_S3TC_DXT5_EXT` | passthrough |
| Hap Q | scaled-YCoCg in BC3 | `GL_COMPRESSED_RGBA_S3TC_DXT5_EXT` | **YCoCg→RGB** |
| Hap Q Alpha | Hap Q + BC4 alpha plane | DXT5 + `GL_COMPRESSED_RED_RGTC1` | YCoCg→RGB + alpha sample |
| Hap R (HapR) | BC7 | `GL_COMPRESSED_RGBA_BPTC_UNORM` | passthrough |

`s3tc` (BC1/BC3) is exposed via `GL_EXT_texture_compression_s3tc` — near-universal on desktop
NVIDIA/AMD/Intel. `bptc` (BC7) is `GL_ARB_texture_compression_bptc` (GL 4.2 core, widely available
as a 3.3 extension). RGTC1 (BC4) is core in 3.0. **All are above-baseline for our pinned GL 3.3
floor and MUST be capability-gated** (see Decision §4), per MultiMon LESSON-ARCH-006 (gate every
optional GPU format with a runtime capability check + a never-crash fallback).

## Decision

1. **Demux with the FFmpeg we already link; do NOT use FFmpeg's HAP *decoder*.** FFmpeg's `hap`
   decoder runs its own texture decompression to RGB(A), throwing away the entire GPU-upload-
   compressed benefit. Instead use FFmpeg purely as a **container demuxer**: `av_read_frame` on the
   `hap`-codec video stream yields the raw compressed HAP packet, which we parse ourselves. This
   reuses our existing dependency (no new vcpkg package, no vendored MOV atom parser — MultiMon
   vendored one only because it was *removing* its media library; we are keeping FFmpeg). Detect at
   load time: if the container's video codec id is HAP, route to the new `HapFileSource`; otherwise
   the existing `VideoFileSource` FFmpeg-decode path is unchanged.

2. **Vendor a minimal Snappy *decompressor* (~300 lines, decode-only, public-domain).** HAP frame
   payloads are Snappy-compressed (or raw, per the section type byte), and newer HAP frames are
   chunked (a decode-instructions section lists N Snappy chunks for multithreaded encode). FFmpeg
   does not expose Snappy. Per the minimalism ladder this is a vendored decode routine under
   `src/source/hap/snappy/`, not a new dependency — but it is **third-party code under `external/`
   discipline** (no hand-edits beyond the vendoring header). Listed in THIRD-PARTY-NOTICES.

3. **Extend the frame-staging contract to carry compressed bytes.** `FrameData` (or a sibling
   `CompressedFrameData`) gains an optional compressed path: a `GpuCompressedFormat` enum (none /
   bc1 / bc3 / bc7 / + ycocg flag) plus the BCn byte blob and per-level byte size. `TextureSource`
   advertises it via a new capability (`producesCompressed`). The `Texture` wrapper gains
   `createCompressed(w,h,fmt)` / `uploadCompressed(bytes)` calling `glCompressedTexImage2D` /
   `glCompressedTexSubImage2D`. `SourceManager::uploadFrameToTexture`
   ([SourceManager.cpp:296](../../src/source/SourceManager.cpp#L296)) branches on the compressed
   capability. The existing RGB(A) path is byte-for-byte unchanged.

4. **Capability-gate every format at startup, never crash.** Query `GL_EXT_texture_compression_s3tc`
   and `GL_ARB_texture_compression_bptc` once at GL init; record in a `GpuCapabilities` struct. When
   a HAP clip needs an unavailable format, fall back to a **CPU BCn→RGBA decompress** path (slow but
   correct) and log it loudly — HAP playback degrades, it does not fail. (Mirrors MultiMon's
   BC7→BC3 / HW→SW fallback discipline; "never crash on an optional GPU feature" is their
   success-metric rule.)

5. **HapQ YCoCg→RGB in a fragment-shader variant**, selected per-source (a `u_ColorSpace` uniform or
   a shader permutation), reconstructing RGB from the scaled-YCoCg BC3 data. Math referenced from the
   HAP spec; one new shader path beside `quad.frag`.

6. **Threading & upload stay on the established split.** Demux + Snappy + chunk-concat run on the
   existing per-source background decode thread; the main thread does the `glCompressedTexSubImage2D`
   upload only (rendering rule: uploads main-thread only). No new thread.

7. **Validate untrusted file fields before allocating.** HAP section sizes and chunk counts come from
   the file; bound them against the packet size before `resize`/upload (MultiMon shipped an OOM fix
   for crafted HAP/MOV/Snappy files — we adopt the bound up front, not as a later CVE).

## Consequences

- **New decode path, not a new pipeline.** Warp, mesh, crop (ADR-040), perform mode, and the render
  loop are untouched — HAP is a source that happens to upload compressed bytes.
- **`.qmap` is unaffected.** A HAP clip serializes as a normal video media path; the loader
  re-detects the codec and routes to `HapFileSource`. No schema change.
- **BCn requires 4-aligned block dimensions.** HAP encoders pad to multiples of 4; the
  compressed-subimage upload must use the padded dimensions and sample the valid sub-rect — note in
  implementation, test with a non-multiple-of-4 source.
- **GL 3.3 floor stays nominal** but HAP *quality tiers* depend on extensions present at runtime;
  the capability gate makes this graceful and observable rather than a hard requirement bump.
- **Vendored Snappy + the FFmpeg-as-demuxer split** are the only new surface; both are small and
  contained under `src/source/hap/`.

## Alternatives considered

- **Use FFmpeg's `hap` decoder (decode to RGBA).** Rejected — discards HAP's entire reason to exist
  (compressed upload); would make HAP *slower* than necessary and pointless next to H.264.
- **Vendor a full MOV atom parser (MultiMon's choice).** Rejected — we already link FFmpeg, so
  demuxing the container is free; a hand-rolled parser is surface we don't need.
- **Add Snappy via vcpkg.** Rejected under the minimalism ladder — a decode-only routine is ~300
  lines and avoids a build-config Change-Boundary edit; revisit only if chunk decode needs the full
  library.

## Acceptance criteria

1. A **Hap** and a **Hap Q** `.mov` load, play, and warp correctly on a quad at 60 FPS (verified by
   running the app — green tests are not the gate).
2. **Multiple simultaneous HAP layers** (≥4 × 1080p, or 2 × 4K) sustain 60 FPS on the dev box where
   the equivalent H.264 layer count does not.
3. On a GPU/driver missing `bptc` (or via a forced-disable test hook), a Hap R clip falls back to CPU
   decompress with a logged warning — **no crash** (the MultiMon capability-gate rule).
4. A crafted/oversized HAP section is rejected before allocation (bounded), not an OOM.
5. Pre-existing `.qmap` files and the H.264/HEVC `VideoFileSource` path are byte-for-byte unchanged;
   `ctest` green; success-metric warp (corner drag / Tab / arrows / F11) intact.

## Implementation contract (A1 → A3 → A4) — decided 2026-06-19

Phasing and the shared types, so the producer (A1) and consumer (A3/A4) agree:

- **A2 (DONE):** `quadmapper::hap::parseHapFrame(const uint8_t*, size_t) -> HapDecodedFrame`
  (`{ HapTextureFormat format; std::vector<uint8_t> textureData; std::vector<uint8_t> alphaPlane; }`),
  in `src/source/hap/`. Pure CPU, bounds-validated, unit-tested.
- **Shared type (DONE, owned by orchestrator):** `FrameData` now carries
  `GpuCompressedFormat { None, BC1_RGB, BC3_RGBA, BC3_YCoCg, BC7_RGBA }` + `isCompressed()`;
  when compressed, `pixels` holds raw BCn bytes and `getDataSize()` returns `pixels.size()`.
  `HapTextureFormat -> GpuCompressedFormat`: RGB_DXT1→BC1_RGB, RGBA_DXT5→BC3_RGBA,
  YCoCg_DXT5→BC3_YCoCg, RGBA_BPTC→BC7_RGBA. (Alpha_RGTC1 / HapQ-Alpha's second plane is
  **deferred in v1** — use the primary plane; note it.)
- **A1 (producer):** `HapFileSource : PlayableSource` — `avformat` demux of the `.mov`, read the
  `AV_CODEC_ID_HAP` video stream's packets on a background thread (do NOT call the FFmpeg HAP
  *decoder*), pass each packet to `parseHapFrame`, and stage a compressed `FrameData`
  (compressedFormat set, `pixels` = `textureData`, width/height from `codecpar`). Mirror
  `VideoFileSource`'s PlayableSource surface + threading. Route in
  `SourceManager::createVideoSource`/`getOrCreateVideoSource`: probe the container's video codec id;
  if HAP → construct `HapFileSource`, else `VideoFileSource`. `SourceType` stays `VIDEO_FILE`.
- **A3 (consumer):** `Texture::createCompressed(w,h,GpuCompressedFormat)` +
  `updateCompressed(bytes,size)` via `glCompressedTexImage2D`/`glCompressedTexSubImage2D`; a
  `Texture` colorspace flag (RGB default / YCoCg) set for BC3_YCoCg.
  `SourceManager::uploadFrameToTexture` branches on `frame.isCompressed()`. A `GpuCapabilities`
  query at GL init (`GL_EXT_texture_compression_s3tc`, `GL_ARB_texture_compression_bptc`); if a
  clip's format is unsupported, **log + skip (black), never crash** (see note below).
- **A4 (shader):** `quad.frag` gains a `u_ColorSpace`/`u_YCoCg` uniform; when the bound texture's
  colorspace is YCoCg, reconstruct RGB from scaled-YCoCg. Renderer sets it from
  `Texture::colorSpace()` per draw.

> **Scope note / ADR deviation to accept:** acceptance criterion #3 said "fall back to CPU
> decompress." v1 implements the capability gate + **graceful skip + log** (no crash) instead — a
> full BCn→RGBA software decoder is deferred (BC1/BC3 s3tc is universal on the desktop GL 3.3
> target; only BC7/HapR is at any real risk). "Never crash" is met; "never fail/CPU-fallback" is the
> follow-up.

## Follow-up (out of scope for this ADR)

- **Convert-to-HAP** (QM-082, proposed): an in-app dialog that transcodes a source to HAP via the
  FFmpeg we already link (`-c:v hap -format hap_q`). MultiMon shipped this; for us it is even cheaper
  since FFmpeg is present. Pairs naturally with playback but is a separate, smaller task.
  **Done (QM-082):** the linked FFmpeg turned out to have no HAP encoder (built without
  libsnappy), so frames are encoded in-house (`src/video/HapFrameEncoder`, stb_dxt vendored in
  `external/stb` with owner approval) and muxed by FFmpeg (`src/video/HapExport`).
