---
name: hap-playback
description: "QUADmapper has HAP/HapQ codec playback (QM-080, ADR-044) — how it's wired and verified."
metadata: 
  node_type: memory
  type: project
  originSessionId: 7af9a884-6117-4eb5-97c2-2dbff43dc4ae
---

QUADmapper plays HAP / Hap Q video (QM-080, Session 21, ADR-044) — the de-facto VJ/projection
codec. It is GPU-compressed BCn, so it slots into the existing pipeline as a decode path + shader
variant (no parallel renderer), per MultiMon's LESSON-ARCH-005.

**Architecture (all under `src/source/hap/` + the source/renderer modules):**
- `hap::parseHapFrame()` — vendored, dependency-free: parses a HAP packet (section header →
  raw/Snappy/complex-chunked) → BCn bytes. Vendored decode-only Snappy in `hap/snappy/`.
  Bounds-validated against crafted files (16 unit tests).
- `HapFileSource` (`SourceType::VIDEO_FILE`) — **FFmpeg as DEMUXER only** (never FFmpeg's HAP
  decoder): `av_read_frame` on the `AV_CODEC_ID_HAP` stream → `parseHapFrame` on a bg thread →
  compressed `FrameData`. Routed via `SourceManager::isHapContainer()` in `createVideoSource`.
- `FrameData` carries `GpuCompressedFormat` (BC1_RGB/BC3_RGBA/BC3_YCoCg/BC7_RGBA); `pixels` = BCn
  bytes when compressed. `Texture::createCompressed/updateCompressed` (`glCompressedTex*`).
- GL capability gate (`renderer/GpuCapabilities`): s3tc (BC1/BC3) + bptc (BC7) queried at GL init;
  unsupported format → log + skip (black), **never crash** (full CPU-decompress fallback deferred).
- HapQ is BC3 flagged `TextureColorSpace::YCoCg`; `quad.frag` `u_ColorSpace==1` reconstructs RGB
  (set in the single `Renderer::renderQuad` call site).

**Verified working** end-to-end: `D:\testing Videos\Hap\5sec_hap.mov` (1920×1080 Hap Q) renders
correct-colored through the real pipeline (`c:\tmp\a5_hapq_shader.png` correct vs `_raw.png` olive).
654/654 ctest. Follow-ups: QM-082 Convert-to-HAP; HapQ-Alpha 2nd plane deferred; CPU-decompress
fallback deferred. See [[test-media-location]], [[quadmapper-ffmpeg-build]], [[perform-stress-harness]].
