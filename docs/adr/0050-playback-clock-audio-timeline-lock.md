# ADR-050: Playback clock, audio from video, timeline lock and Ping-Pong

**Date:** 2026-09-26
**Status:** Accepted (owner approved PRD steps 1–4, 2026-09-26). This is the transport-and-audio subset of the "media pipeline v2" the foundation audit reserved ADR-050 for. PBO uploads, YUV in the shader, d3d11va and stride-aware formats are left for a later ADR.
**Deciders:** Jones

## Context

Three PRD MVP items are open: audio (US-012, P0: "audio playback from video files, per-source volume, master volume, mute per source"), timeline lock (§4.1.3), and the Ping-Pong loop mode (US-006, §4.1.3). Research (2026-09-26) found:

- **Audio is built but unused.** `src/audio` is compiled and unit-tested, but nothing outside it references `AudioMixer`/`AudioDecoder`/`AudioSource`/`AudioDevice`.
  - The mixer callback is real-time safe: it reads a copy-on-write source snapshot with no lock and no allocation.
  - `AudioDecoder` opens its own FFmpeg demux on the file and resamples with swresample on a background thread.
- **No clock object.** Every `VideoPlayer`/`HapFileSource` computes `position += deltaTime * speed` from the frame's `glfwGetTime()` delta. Rounding accumulates per source, and nothing corrects it.
- **Loop restarts by seeking.** At the end of the video, a loop does `seek(0); play()`.
- **Ping-Pong is only a loop.** `PING_PONG` is coerced to `LOOP` (QM-088) because the decoder only goes forward. The value still round-trips in `.qmap`.
- **`TimelineSyncManager` is dead.** It holds raw `VideoPlayer*` pointers and has no call site and no test.
- **Pausing unseen sources has no audio counterpart.** `releaseUnusedSources` pauses sources that are off screen; nothing pauses their audio.
- **`.qmap` saves only loop mode and speed** per source.

## Decision

### 1. `PlaybackClock` — video is the master, and it follows the wall clock
- A small value type (in `src/video`, no GL): `{anchorMedia, anchorWall (steady_clock), speed, direction, running}`.
  - `position(now) = anchorMedia + (now - anchorWall) × speed × direction`, while running.
  - Play, pause, seek, a speed change and a direction change each re-anchor, so no rounding accumulates. Media time tracks the wall clock exactly, by construction; this is the PRD target of < 1 frame/hour drift.
- Loop and Ping-Pong wrap **analytically** from the clock (`position mod duration`, or a reflected triangle wave). The decoder is repositioned to follow the clock, rather than the clock being restarted.
- `VideoPlayer` and `HapFileSource` replace their dt accumulation with a `PlaybackClock`. `popDueFrames` keeps selecting frames against `position(now)`. The existing `update(deltaTime)` signature stays; the delta is ignored for timing.
- **Why video is the master, not audio:** a projection show often has many videos, most of them silent, and some locked together. One wall-clock-driven model serves them all. Audio is slaved (§2), so a silent source and a sounding source behave the same way.

### 2. Audio from video files
- **One app-wide `AudioMixer`,** opened on the preferred output device (default: the system default). If the device fails to open, the app logs it and runs silent, never crashing.
- **Each video source with an audio stream gets an `AudioAttachment`:** an `AudioDecoder` (its own demux; acceptable for local files) plus an `AudioSource` registered with the mixer. It is created and destroyed with the source, and the source GC releases it.
- **Sync:** audio is slaved to the source's `PlaybackClock`. The attachment tracks the media time of the next sample to be played (decoder timestamps plus samples consumed by the callback, minus device latency).
  - Once per frame, the main thread computes `drift = audioTime − clock.position(now)`.
  - **|drift| > 80 ms** (after a seek, a loop wrap or an underrun): hard resync. Flush, seek the audio decoder to the clock and preroll.
  - **2 ms < |drift| ≤ 80 ms:** soft correction. The callback drops or repeats one sample in every 1000 (a 0.1 % rate change, inaudible) until the drift is within 2 ms.
- **Transport:** audio plays only while the source plays forward at 1×.
  - At any other speed, and on the reverse leg of Ping-Pong, it is silent. The UI says so.
  - Tape-style varispeed is a later additive feature.
  - Pause, seek, loop and pause-while-unseen all act on the audio too. When a source is off screen and paused, its audio pauses as well.
- **Controls:**
  - Per-source volume (0–1) and mute, in the video/source panel.
  - Master volume and mute, in an always-visible transport strip and the Output menu.
  - The audio output device, in Preferences.
- **Audio-only media** (mp3/wav) is out of scope: the PRD asks for audio from video files.

### 3. Timeline lock
- `TimelineSyncManager` is deleted. It is replaced by one project **master timeline**: a `PlaybackClock` owned by the app, with play, pause and restart.
- A source with `timelineLocked` set follows the master: `position = masterTime mod its duration` (or reflected, for Ping-Pong). Locked sources therefore stay phase-locked indefinitely, and their own transport controls show "locked to master" and are disabled.
- Unlocked sources keep their own clocks.

### 4. Ping-Pong
- The clock reflects at each end. The decoder has to deliver frames backwards on the reverse leg:
  - **HAP (intra-only):** exact. Seek and decode the single needed frame.
  - **FFmpeg long-GOP:** backward GOP decoding. Seek to the keyframe before the target, decode forward into a bounded frame cache, and present from the cache in reverse, then step to the previous GOP.
    - The cache has a memory budget (default 512 MB).
    - If one GOP at the file's resolution cannot fit, Ping-Pong for that source falls back to Loop. The UI explains why and suggests HAP or an all-intra encode. It never stalls or grows memory without bound.
- Saved `"pingPong"` values load as real Ping-Pong again. Files are unchanged.

### 5. Persistence (`.qmap` v3.3, additive, ignored by older builds)
- Per source: `volume`, `muted`, `timelineLocked`.
- Project: `"audio": {"masterVolume", "masterMuted"}`.
- The device is an app preference, not saved in the project.
- Missing fields default to volume 1, not muted, not locked, and master 1, not muted. Existing files therefore play exactly as they do today, now with sound.

## Consequences
- ✅ Closes US-012, timeline lock and Ping-Pong. Media time cannot drift from the wall clock, and locked sources cannot drift from each other.
- ✅ A silent and a sounding source share one model. Audio can never pull video off its frame pacing.
- ✅ Reuses the real-time-safe mixer that already exists.
- ❌ The audio device clock drifts slightly from the wall clock, so audio is corrected continuously; the correction is inaudible at 0.1 %.
- ❌ Audio is silent at non-1× speeds and on the reverse leg.
- ❌ Ping-Pong on long-GOP files is limited by the cache budget; HAP or all-intra files are recommended for reverse playback.
- ❌ Every file with audio is demuxed twice; this is fine for local files.

## Alternatives considered
1. **Audio as the master clock (ffplay-style).** Rejected: it gives silent and sounding sources different models, and it makes timeline lock depend on which sources have audio.
2. **Repair `TimelineSyncManager`.** Rejected: it holds raw pointers, restarts by seeking instead of computing positions from the clock, and has no tests.
3. **Cut audio from the MVP.** Rejected by the owner (US-012 is P0).
