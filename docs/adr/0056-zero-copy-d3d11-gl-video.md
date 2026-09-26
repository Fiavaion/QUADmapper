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
