# ADR-055: Video pipeline v2 — hardware decode, NV12 on the GPU, asynchronous uploads

**Date:** 2026-09-26
**Status:** Accepted (owner: "get this development done", 2026-09-26). This is the rest of the "media pipeline v2" the foundation audit reserved under ADR-050; the playback clock and audio parts shipped as ADR-050.
**Deciders:** Jones

## Context

The PRD asks for 16 simultaneous videos. QM-107 measured 16 × 1080p H.264 at about 2.5 fps (Release).
- **Decoding is in software.** A 20 Mbit/s 1080p25 clip takes 35–60 ms per frame on one thread, and two threads are all 16 streams can share.
- **Frames are converted to RGB24** with swscale and copied twice (decoder → player → source).
- **Uploads block the main thread,** about 4.7 ms per 1080p frame.

HAP, which the GPU decompresses, already reaches about 98% of frames at 16 streams. The FFmpeg in vcpkg ships the D3D11VA hardware-context headers. Whether the build enables the D3D11VA hardware accelerator must be confirmed at runtime.

## Decision

1. **Hardware decode where available.**
   - H.264, HEVC, VP9 and AV1 decode through D3D11VA (`av_hwdevice_ctx_create`, `get_format`). The decoded surface is copied to system memory as NV12 with `av_hwframe_transfer_data`.
   - Software decode stays as the fallback, used automatically and per stream when the GPU refuses (profile, resolution, too many sessions). The Video Player shows "hardware" or "software" for each clip.
   - If the installed FFmpeg lacks D3D11VA, the implementer stops and reports. Changing `vcpkg.json` features needs the owner's approval, because it rebuilds FFmpeg.
2. **Keep NV12 (and YUV420P from software decode) all the way to the GPU.**
   - Upload the planes as R8 and RG8 textures (1.5 bytes per pixel instead of 3). Convert to the source's RGBA texture in a small GPU pass, using the stream's colour matrix (BT.601 or BT.709) and range (limited or full).
   - Every consumer — quads, 3D objects, previews, thumbnails — still samples one RGBA texture, so `quad.frag` and the mesh shader are unchanged.
   - swscale remains only for formats the GPU pass doesn't handle.
3. **Asynchronous uploads.**
   - Pixel buffer objects (a small ring per source) so `glTexSubImage2D` returns at once.
   - The decode thread writes into a mapped PBO where that is safe; otherwise one copy into it.
   - Frames move by handle, with no player → source copy.
4. **Frame timing is unchanged:** the PlaybackClock (ADR-050) still picks which frame is shown. Seek, Ping-Pong reverse decode and the EOF drain keep their tests.
5. **Gates.**
   - Colour is correct: a test clip with known colour bars, in BT.601/709 and limited/full range, matches reference RGB within ±2 per channel.
   - Measured with `--bench` (Release) on this machine: 16 × 1080p H.264 presents ≥ 95% of expected frames at ≥ 55 fps. The main-thread upload time per frame is reported before and after.
   - All existing playback tests pass. The stress harness shows no leaks, including the D3D11 device and the hardware frame pool.

## Consequences
- ✅ H.264 and HEVC shows can run many streams; the CPU mostly waits.
- ✅ Half the upload bandwidth, and no RGB conversion on the CPU.
- ❌ More moving parts: a hardware device per app, a frame pool per stream, and a fallback path to keep tested.
- ❌ Zero-copy D3D11 → GL interop (`WGL_NV_DX_interop2`) is not attempted. The copy to system memory stays; it can be a later step.

## Alternatives considered
1. **Recommend HAP only.** Rejected: most show media arrives as H.264 or HEVC.
2. **Zero-copy interop now.** Rejected for v2: it is vendor-specific and adds risk. The measured gain first comes from hardware decode plus NV12 plus PBOs.
