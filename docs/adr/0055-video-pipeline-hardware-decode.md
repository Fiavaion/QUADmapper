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

## Amendment (implementation, 2026-09-26)
- **D3D11VA is in the vcpkg FFmpeg** (7.1: `h264/hevc/vp9/av1_d3d11va` hwaccels). No `vcpkg.json` change was needed. On the dev machine the device lands on the RTX 3080 Laptop GPU, the same GPU as the GL context.
- **One device, kept until exit.** Creating and destroying a D3D11 device per clip leaked about 1.5 MB of driver memory each time: 40 stress cycles re-creating a clip grew the working set by 65 MB, against 25 MB with one device. The device is created at the first hardware decode and never released. Releasing it at exit would race the driver's own unload, and the OS reclaims it anyway. Giving each stream its own device was also tried; it kept 8 cores busy and was slower.
- **Readback.** `av_hwframe_transfer_data` holds the shared device's lock while it waits for the GPU copy and while it copies the pixels. With 16 streams, each stream queued behind the others: about 20 ms per frame, and 85–93% of frames presented. Instead, each stream copies its surface to its own staging texture and polls `Map` with `DO_NOT_WAIT`, holding the lock only for the D3D11 calls. That raised presented frames to 99–102%. The frames land in a per-stream buffer pool (NV12, or P010 for 10-bit).
- **Fallback.** The first frame decodes at open, so a stream the GPU refuses (profile, size, sessions) starts in software. There are two triggers later: `get_format` falling back to a software format, or a failed readback. Either one reopens the codec with two-thread software decoding. Decoding restarts from the keyframe before the same frame, so no frame is skipped or repeated. The test hook is `DecodeOptions::failHardwareAfterFrames`.
- **§3 uploads, as built.** The decode thread does not write into PBOs. It would need about 400 MB of mapped buffers kept supplied to 16 decoders, with a back-pressure deadlock risk. Instead:
  - The main thread maps a PBO, and a copier thread in the renderer fills it. A mapped buffer's memory is ordinary memory, so map, unmap and GL still happen only on the main thread.
  - After each upload, the frame queued next is staged. In steady playback the main thread then only maps, unmaps and issues `glTexSubImage2D`.
  - At 16 × 30 fps, 85% of prefetches hit. A mismatch, from a seek or a dropped frame, copies anew on the same path.
  - Main-thread cost is 0.3–0.8 ms per 1080p frame, depending on machine load. It was 4.3–12.8 ms with RGB24.
- **Fixed GL objects per video source.** Each source gets three plane textures and two PBOs, named when it registers. Storage is sized at the first frame. The stress harness compares GL object counts every cycle.
- **Colour.** Untagged video counts as BT.709 when HD and BT.601 when SD. JPEG-derived pictures are always BT.601. The old swscale path always used BT.601 limited range, so tagged or HD BT.709 clips now show their correct, slightly different colours. MPEG-2 cannot signal full range. Thumbnails and cameras keep swscale's defaults.
- **Gate results.** Measured with `--bench`, Release, 2 × 1080p outputs, uncapped, while other agents were using the same machine:

  | Clip | Before (fps, presented) | After (fps, presented) |
  |---|---|---|
  | greenCrystals, 16 × H.264, 25 fps, 20 Mbit/s | 1.8–2.7 fps, 6–11% | 70–101 fps, 95.6–100.9% |
  | 5sec.mp4, 16 × H.264, 30 fps | 3.2–3.6 fps, 11–12% | 42–70 fps, 82–97% |
  | 16 × HEVC (x265, 25 fps) | 2.4 fps, 5% | 37–57 fps, 80–88% |

  - The gate holds for the reference clip.
  - 30 fps clips and HEVC meet it only on a quiet machine. The remainder is tracked in TD-19.
  - `WGL_NV_DX_interop2` zero-copy is still not attempted. It is the next lever and needs its own ADR.
