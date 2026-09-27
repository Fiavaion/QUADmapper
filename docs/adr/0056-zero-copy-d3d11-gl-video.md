# ADR-056: Zero-copy hardware video — D3D11 frames shared with OpenGL

**Date:** 2026-09-26
**Status:** Accepted (owner: "get this development done", 2026-09-26)
**Deciders:** Jones

## Context

ADR-055 decodes on the GPU with D3D11VA, but it still copies each frame back to system memory and uploads it to OpenGL again. For 8-bit streams that meets the PRD target. For 10-bit HEVC it doesn't:
- A P010 frame is 6.2 MB, and its upload stalls the GL context for 2.1–2.3 ms per frame.
- The graphics engine is only about 34% busy while the copy engine carries the transfers.
- Measured: 16 × 1080p25 10-bit HEVC reaches 30–37 fps; the gate is ≥ 55 fps.

No lossless variant of the copy path reaches the gate on the development machine (ADR-055 Amendment 2). The copy has to go.

## Decision

1. **Keep decoded frames on the GPU and share them with OpenGL through `WGL_NV_DX_interop2`.** NVIDIA, AMD and Intel all implement it on Windows.
   - Each hardware video source owns one D3D11 render-target texture, **RGBA8, or RGB10_A2 for 10-bit sources**, created on the app's single D3D11 device and registered once with `wglDXRegisterObjectNV` as a GL texture.
   - The decoder's surface (from the NV12/P010 decoder pool) is converted into that texture on the GPU by an `ID3D11VideoProcessor` blit, which applies the stream's colour matrix and range. `YuvConverter`'s BT.601/709/2020 × limited/full coverage must be matched; the colour-bar tests are the gate.
   - OpenGL then samples the shared texture directly. Access is bracketed with `wglDXLockObjectsNV`/`UnlockObjectsNV` around the frames that read it, and the lock is held as briefly as possible.
   - The source's RGBA texture contract is unchanged, so quads, 3D objects, previews and thumbnails keep working as they do now.
2. **Threading.**
   - The D3D11 immediate context is used under the existing device lock; the video processor blit runs where the readback runs today.
   - GL calls, including interop lock/unlock and the per-source texture binding, stay on the main thread. A small handoff (a frame generation number per source) tells the main thread when a new frame is ready. No GL call moves off the main thread.
3. **Fallback is automatic, per app and per stream.**
   - If the extension is missing, registration fails, the video processor rejects the format, or interop errors at run time, the stream uses the ADR-055 readback path as today.
   - The Video Player shows "hardware (zero-copy)", "hardware" or "software".
   - A test hook forces each fallback.
4. **Lifetime.**
   - Interop objects are unregistered before the D3D11 texture or GL texture is released, and all of them before the GL context is destroyed (shutdown order, QM-115).
   - A source released or re-created (GC, undo restore, relocation, stress churn) leaks nothing. The stress harness's GL counts plus a D3D11 live-object count stay flat.
5. **Gates.**
   - Quiet-machine Release `--bench`, 2 × 1080p outputs: 16 × 1080p25 10-bit HEVC ≥ 95% of frames at ≥ 55 fps.
   - 8-bit HEVC and H.264 do not regress.
   - Colour bars within ±2 on the zero-copy path.
   - All playback tests (seek, Ping-Pong, timeline lock, fallback) pass on both paths.

## Consequences
- ✅ Removes the readback and the upload for every hardware stream, not just 10-bit. The CPU and copy engine are freed for more streams.
- ✅ 10-bit sources keep their precision through the conversion (RGB10_A2).
- ❌ A vendor-extension path to maintain next to the readback path. Both stay tested through the fallback hook.
- ❌ Interop locking can serialise with the output frames if it is held too long; locks must be scoped per frame.

## Alternatives considered
1. **Upload 10-bit as 8-bit, dithered.** Rejected as the default: it drops precision before the colour conversion. It may remain a later per-source option.
2. **Fence-recycled PBOs.** Measured: 10-bit rose to 35–37 fps, but H.264 lost 25–40%. Rejected.
3. **ANGLE / D3D11-backed GL.** Rejected: it would replace the whole GL stack.

## Amendment (implementation, 2026-09-27)

Built as decided in spirit, with the following changes. Each was forced by a measurement on the development machine (RTX 3080 Laptop, NVIDIA driver, one GPU for D3D11 and GL).

- **No `ID3D11VideoProcessor`. The planes are shared and `YuvConverter` converts them.**
  - Each source shares two textures with GL, luma (R8, or R16 for 10-bit) and chroma (RG8 or RG16). A small compute shader copies a decoded frame's planes into them unchanged (HLSL compiled at run time by the system's `d3dcompiler_47.dll`, loaded dynamically). `YuvConverter` then draws them into the source's RGBA texture with the same pass, matrix and range as the upload path.
  - Why: the video processor refused BT.2020 (it only offered a 2020→709 gamut change `YuvConverter` does not make), and its colours only approximated the upload path (±2). Sharing the planes gives the upload path's pixels exactly: the first frame of 8-bit HEVC, 10-bit HEVC and H.264 clips differs by 0 from the same frame read back and uploaded (`ZeroCopyFirstFrame.*`). Colour bars are within ±1 for all of BT.601/709/2020 × limited/full at 8 and 10 bits (`DxInterop.ColourBarsMatch…`).
  - The consumer texture stays RGBA8 as on every other path; the RGB10_A2 intermediate was not needed.
- **The shared textures must be created `D3D11_RESOURCE_MISC_SHARED`.** Without it NVIDIA's interop gave GL a copy taken at registration, so every source stayed black. This was the bug the first attempt stopped on. `DxInterop.EveryNewFrameReachesTheTexture` guards it.
- **DxInterop has its own D3D11 device, not the decoders' (reverses §2's "under the existing device lock").**
  - Sharing the decoders' immediate context made the main thread queue behind 16 decoders: FFmpeg's lock waits up to 190 ms. Without that lock, `Flush` and `wglDXUnlockObjectsNV` still stalled 48–146 ms, and frames of up to 290 ms appeared at every loop of a short clip.
  - Now each stream copies its decoded slice into a texture of its own pool (12 textures, created `MISC_SHARED_KEYEDMUTEX`) on its own decode thread. The renderer's device, on the same adapter and used only by the main thread, opens that texture (`OpenSharedResource`, cached per pool texture) and reads it under the keyed mutex. The main thread never touches the decoders' device or FFmpeg's lock.
- **Decoder threads wait for their copy, as the readback path does.** The pool copy sits in the decoders' one GPU queue behind every stream's decoding. A renderer that took an unfinished copy waited on all of that: after each loop of `5sec.mp4` the app ran at about 24 fps for a second, and delivery fell to 85–90%. The decode thread now polls an event query (lock held only for the calls, 0.5 ms sleeps) and queues the frame once the copy is done. The poll must allow flushing: with `D3D11_ASYNC_GETDATA_DONOTFLUSH` the event never completed after the keyed-mutex release.
- **Surface reuse.** The decoder's own surfaces are released as soon as the slice is copied (as in ADR-055), so the decoder never waits on the renderer. A pool texture returns to the pool only when no frame holds it. Its keyed mutex keeps the decoder's next write after the renderer's read on the GPU.
- **Fallback, as §3.**
  - An extension missing, a device or shader that cannot be made, a failed registration, a format that cannot be shared, a frame that cannot be opened or locked, or a failed interop lock: that stream (or, for app-wide causes, every stream) goes back to the ADR-055 readback path. Frames already on the GPU are read back first, so there is no gap and no black frame.
  - Logged once: app-wide at `info`/`warning`, per stream by `VideoDecoder::stopZeroCopy`. The Video Player shows "hardware (zero-copy)", "hardware" or "software", with the reason in a tooltip. `DxInterop::TestHooks` forces each case.
- **Lifetime.**
  - A source's target unregisters its GL textures before releasing the D3D11 textures. It holds the interop device alive, so the device closes after the last target, with the main context current (shutdown and project replace already destroy sources that way).
  - `DxInterop::liveObjects()` counts the per-source D3D11 objects and registrations. `VideoDecoder::liveGpuFrames()` counts pool textures. The stress harness now fails if either is non-zero once its video source is removed.
- **Link changes: none.** `d3d11.dll` and `d3dcompiler_47.dll` are loaded at run time; only system headers are included.

**Measurements.** Release `--bench=videos16`, 2 × 1080p outputs, uncapped, 10 s. **The machine was never quiet**: VS Code, a WSL VM, Docker, Defender and other agents' compilers (0–12 `cl.exe`) ran throughout, with CPU at 20–100%. For part of the session other processes also held about 6 of the GPU's 8 GB, and both paths collapsed then; those runs are excluded. The A/B rows used a temporary switch (not committed) to force the readback path in the same binary, run back to back under the same load.

| 16 × 1080p clip | Zero-copy, final build (3 runs) | Readback path, same load (A/B) | Before (ADR-055, quiet machine) |
|---|---|---|---|
| 10-bit HEVC (`hevc10_1080p.mp4`, 25 fps) | 103–114 fps, 99.9–100% | 20–27 fps, 77–97% | 30–32 fps, 95–96% |
| 8-bit HEVC (`hevc_1080p.mp4`) | 96–131 fps, 91.4–99.9% (91.4%: one run with a 194 ms stall under load) | 60–88 fps, 98.7–100% | 162 fps, 100% |
| H.264 `5sec.mp4` (30 fps, loops every 5 s) | 90–145 fps, 98.6–99.8% | 52–72 fps, 92–98% | 107–116 fps, 98.9–99.7% |
| H.264 greenCrystals (25 fps, 20 Mbit/s) | 98–126 fps, 100% | 63–95 fps, 100–103% | 163 fps, 100.5% |

Main-thread time in `SourceManager::updateAll` for the videos, per rendered frame: 1.0–2.3 ms zero-copy against 4–17 ms on the readback path under the same load. Process CPU: about 9% of the machine against 11–19%. In the calmer moments of an earlier round of the same design, zero-copy reached 157–195 fps at 100% on every clip.

The 10-bit gate (≥ 55 fps, ≥ 95%) is met with a wide margin even under load. No clip regresses against the readback path measured beside it. The absolute fps is below the quiet-machine ADR-055 numbers only because of the load, so a quiet-machine rerun is still owed.

**Not verified:** AMD and Intel drivers (only NVIDIA was available); a GPU driver reset during playback; a laptop where the GL context and the decoders land on different GPUs (the device is created on the decoders' adapter, and registration or open would then fail into the fallback).
