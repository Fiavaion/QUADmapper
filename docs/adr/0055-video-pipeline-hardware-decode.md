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

## Amendment 2 (10-bit investigation, 2026-09-26)
The gate misses only for 10-bit HEVC: 16 × 1080p25 P010 runs at 30–32 fps with 95–96% of frames presented. Measured on the quiet machine (Release, 2 × 1080p outputs, RTX 3080 Laptop at PCIe 3.0 x8, resizable BAR on).

- **Where the time goes: the plane upload.**
  - GL timer queries around each conversion: the upload takes 2.1–2.2 ms of GPU per P010 frame, the pass 0.04 ms, the mipmaps 0.13 ms.
  - Nsight Systems (OpenGL + WDDM + GPU metrics) shows the same 2–3 ms spans back to back through the frame, charged to the next GL call after the upload, whichever it is (the mipmap, or a `glClear` once the passes were batched). The graphics engine is active only about a third of the time (GR Active 34%) and the async copy engine 33–45%: the GPU mostly waits on the transfers.
  - Skipping only `glTexSubImage2D` gives 136 fps. Uploading only P010's high bytes (NV12-sized, SSE2 extraction on the copier) gives 65–68 fps with 100% of frames presented.
- **Not the cause.**
  - The 16-bit formats: byte-pair RG8/RGBA8 textures upload just as slowly.
  - The D3D11 readback: skipping the GL side entirely gives 161 fps and 100% of frames.
  - The conversion pass and the mipmaps.
  - The unpack alignment, the buffer usage hint (DYNAMIC, STREAM_COPY, STATIC), and the size or count of the pixel buffers (≤ 2 MB bands of rows).
- **Tried, none reaches the gate.**
  - All uploads of a frame first, then all passes: 23 fps.
  - Uploading the next frame ahead into a second set of plane textures: 26 fps. At that frame rate there is no spare frame to upload in.
  - A linear copy into a device-local buffer before the upload: 20 fps.
  - Reusing each pixel buffer in place once a fence says the GPU has read it, instead of orphaning it every frame. Paging operations fall 3× and 10-bit rises to 35–37 fps with 100% of frames. But H.264 drops 25–40%, because unsynchronised maps stall NVIDIA's driver thread. Not kept.
- **Why a lossless fix cannot reach the gate on this path.** 10-bit 4:2:0 needs at least 3.9 MB a frame, 25% more than NV12. Adding an upload of that residual size to the high-byte path drops it from 68 to 43 fps.
- **The way out is one of:**
  - (a) Zero-copy D3D11→GL through `WGL_NV_DX_interop2`. It removes the readback and the upload, and needs its own ADR: it moves work across the decode/render thread boundary.
  - (b) Uploading 10-bit sources as 8-bit, optionally dithered. That is an image-quality decision for the owner.

  TD-19 tracks the choice.

## Amendment 3 (Loop without a codec restart, TD-30, 2026-09-27)
16 × `5sec.mp4` (1080p30, 5 s) lost frames at every loop: decode lag p99 228 ms on the quiet machine, 0.3–0.6 s under load.

- **Cause.** At the end of each Loop pass the decoder drained, then `avcodec_flush_buffers` and a seek to 0. The flush makes FFmpeg's H.264 and HEVC decoders set up again at the next keyframe (`get_format` is called again). With D3D11VA that means a new GPU decoder and surface pool, created one stream at a time on the shared device, ~50 ms each. Instrumented: every stream reached the end with 7 frames queued, then waited 0.8 s (under load) for its next frame, and all 16 got it within 2 ms of each other. The demuxer seek took 1–30 ms.
- **Change.** In Loop, when the demuxer reaches the end, it goes back to the clip's first frame (by time, or by byte where a time seek lands late, as MPEG-TS does). That packet is sent straight after the last one, as a stream with a keyframe there, if it is a clean entry point: a keyframe, and for H.264/HEVC an IDR or BLA picture (other keyframes may be followed by pictures that refer back across them). The frames held for reordering come out first; the first frame whose time goes back starts the next leg. Otherwise the old drain-and-restart runs.
- **Unchanged.** The PlaybackClock and every frame's due time (ADR-050); Ping-Pong, which still restarts at each reflection and seeks per group on the reverse leg; seeks; the readback, zero-copy and software paths all take frames as before.
- **Evidence.** `get_format` runs only at open. `VideoDecoderLoopSeam.*` and `HardwareLoopSeam.*` (readback and zero-copy) check that the next pass is queued before the seam with contiguous due times and no restart (`VideoDecoder::decoderRestarts`). Release A/B under load: decode lag p99 323–581 → 33 ms, worst stream 95.3–99.0% → 99.6–99.9%; greenCrystals and 10-bit HEVC unchanged. Numbers are in TD-30 (resolved).

## Amendment 4 (no blocking open, teardown or reposition on the main thread, TD-35, 2026-10-01)
Under load single main-thread calls blocked every output for seconds (TD-35): repositioning joined the decode thread, a removed source's decode threads were joined and its decoders freed, files and hardware decoders were opened, and the zero-copy interop device was created at the first frame.

- **Reposition.** `VideoDecoder::startPlayback` no longer stops the decode thread: it empties the queue and hands the plan over under the queue's lock; the decode thread positions itself (seek, reverse-leg set-up) at its next step. Decoding toward a replaced plan is interrupted at the same points a stop interrupts it, and its frames are dropped: a frame decoded for it is checked again under the queue's lock before it is queued, and the frames the zero-copy fallback had taken out of the queue to copy to memory (`leaveGpu`, ADR-056) are dropped instead of being put back when a reposition landed meanwhile. (Review fix: they were put back in front of the new plan's frames, held them back and froze the clip on a wrong frame, 30 of 30 tries on a 4K clip.)
- **Open.** `SourceManager::createVideoSource` registers the `VideoFileSource` at once, not ready, and opens a `VideoDecoder` on an opener thread (`BackgroundWorker`); `updateAll()` hands it over (`VideoPlayer::beginOpen`/`finishOpen`). The transport works while the file opens and its clock runs, so play, seek, loop, speed and lock given meanwhile take effect where the clock is when decoding starts. A missing file is still refused (null id, kept as missing media), and so is a file the header probe finds is not a container FFmpeg reads or holds no video stream. The probe reads the container header only (it also routes HAP clips; HAP clips still open on the main thread). A file that passes the probe but whose decoder then fails to open (or whose open throws) leaves its source empty, not ready, with `getErrorMessage()` set and the transport it was given kept, and lists it in `takeFailedOpens()`; the editor turns it into missing media under its own id (`adoptFailedOpens`): black on outputs, listed in Missing Media, saved back with that transport. Until a source has a picture (while it opens) its surfaces draw black in every view, never the renderer's white default (`buildQuadTextureMap`); a replacement (Convert to HAP) shows the original through `setStandIn` until its first frame, when the original has a picture. A Loop or Ping-Pong clip played while it opens, for longer than it lasts, starts where it would have been had its length been known (`PlaybackClock::setDuration` wraps the time run meanwhile). (Review fixes, 2026-10-01: an opening source drew white on the projectors; an undecodable file stayed white forever, was accepted by Missing Media relocation, was not listed there and was saved as stopped; an exception in the opener left the source opening forever.)
- **Teardown.** `removeSource`/`clear` hand video and HAP sources to a reaper thread, which shuts them down (decode and demux threads joined, FFmpeg and D3D11 freed). Their GL state (texture, upload state, shared planes) is released on the main thread, one source a frame. A source removed while opening is dropped when its open finishes. The destructor joins the opener, then the reaper, before what they use is destroyed. `AudioEngine` takes a released attachment out of the mix on the control thread (`AudioAttachment::leaveMix`) and frees it on its own reaper; the audio callback is unchanged (TD-14).
- **Zero-copy device (ADR-056).** The opener thread prepares the interop D3D11 device and its plane-copy shader (`DxInterop::prepare`) once the first stream that leaves frames on the GPU is open; the first zero-copy frame only opens it for OpenGL. `SourceManager::prepareGpu()` at start-up builds the YUV shader and checks interop support (GL only, no D3D11). Creating and opening the interop device at start-up was measured and withdrawn: on a saturated GPU the GL open took 2.8 s (up to 15.5 s) in every process, with or without video.
- **Threading and lifetime rules.** GL objects are created and freed on the main thread only. Workers touch only objects handed to them whole (an opening decoder, a removed source, a released attachment); nothing the main thread can still reach. The opener's result is published with a release/acquire flag. Every worker is joined before what its tasks use is destroyed. `finishBackgroundWork()` and `finishReleasing()` wait for the workers, for tests and leak checks only.
- **Evidence.** `MainThreadTimings` reports main-thread time per call (stress log, bench JSON `main_thread_ms`, raw samples to `QM_TIMINGS_DUMP_DIR`); the app records only under `--stress` and `--bench`, so a show takes no lock and keeps no samples. Release, realistic load (the three stress harnesses, bench_smoke and `--bench=videos16` together), pooled p50 / p99 / max: createVideoSource 3.2 / 25.8 / 31 ms (before: 282 / 883 / 883 per process), removeSource 0.02 / 0.16 / 0.4 ms (10 / 23 / 23), reposition 0.08 / 0.38 / 2.5 ms, syncAudio 0.05 / 0.74 / 38 ms, clear 0.12 / 0.99 / 2 ms (0.3 / 106 / 106). Stress harnesses 21/21 (realistic) and 6/6 (heavy); `--bench=videos16,quads256` unchanged (interleaved runs). Tests: `AsyncSourceLifecycle.*`, `VideoDecoderReposition.*` (the storm, a frame meeting a reposition right before the queue, a reposition during the zero-copy fallback), `OpeningMediaTest.*`, `UndecodableMediaTest.*`, `MissingMediaTest.RelocateRefusesAFileThatIsNotAVideo`, `PlaybackClock.ADurationArrivingLateWrapsTheTimeRunMeanwhile`, `MainThreadTimingsTest.*`, `AudioSyncTest.AReleasedAttachmentLeavesTheMixAtOnceAndIsFreedOffTheControlThread`. What is still above one frame is listed in TD-35.
- **Stress harness (2026-10-02).** With the open on the opener thread, the open ran inside the harness's 5 s per-cycle wedge budget, so a slow open under load read as a wedge (1 round in 100, no thread blocked). The budget now restarts when the open ends; the open has its own 60 s bound, a failed open fails at once, and `stress_slow_open_smoke` delays every open past the budget. The warm-up cycle has a 30 s budget for the one-off GPU set-up (the first zero-copy device open, reported in every run); later cycles keep 5 s. Private bytes are logged beside the working set; the bound stays on the working set (private bytes swing as much under load). 100/100 rounds plain and 50/50 beside a looping `--bench=videos16` after the change.
