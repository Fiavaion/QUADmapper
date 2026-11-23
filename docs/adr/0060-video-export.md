# ADR-060: Video export to MP4, one file per output

**Date:** 2026-11-23
**Status:** Accepted (owner, 2026-11-23: "One file per output", "H.264 MP4")
**Deciders:** Jones
**Relates:** ADR-040 (sources), ADR-050 (audio), ADR-054 (one projector render per frame), ADR-039 (outputs and scenes)

## Context
- The owner asked to "render out a video of the mapping so a user can write the file to a media player for use without the pc". The length is set by the user, or is the video's own length when only one video is mapped.
- The owner chose:
  - one file per output, each at the output's own resolution and rotation, so each projector's player gets its own file;
  - H.264 in MP4, with the mapped videos' sound as AAC.
- Live playback runs on the wall clock and drops late frames. An export has to show every frame, whatever the machine's speed.

## Decision
1. **File > Export Video...** (`ui/dialogs/ExportVideoWindow`) sets the export:
   - **Outputs:** the enabled ones are ticked at first.
   - **Length:** when exactly one distinct video is mapped in the chosen outputs' scenes, its playing length (duration / |speed|) is offered. Otherwise the user sets the seconds. The rule is `singleVideoSeconds` in `editor/VideoExportPlan.hpp`.
   - **Frame rate:** 25, 30 (the default), 50 or 60.
   - **Sound:** on or off.
   - **Where:** a file name and a folder (the project's folder at first). Files are named `"<name> - <output>.mp4"`, with " (2)" and so on added where two outputs would get the same file (Windows ignores case; `exportFileNames`).
   - Export is disabled under the perform lock, because the projectors show the export while it runs.
2. **A snapshot, not the live show** (`app/VideoExport`):
   - The show is copied as it would be saved (`syncEditorToProject`) and loaded into a private EditorState and SourceManager (`syncProjectToEditor`, with no output manager). Scene, quad and source ids are kept, so the live outputs' routing and slices apply as they are.
   - The live show keeps playing and stays editable. Later edits are not in the file.
   - The test grids of quads without media are lent from the live editor, because they are not saved.
   - The snapshot waits until its media has opened and its 3D objects' meshes are parsed (`adoptLoadedMeshes`), for at most 30 s.
3. **An injected clock.**
   - `SourceManager::setTimeSource` drives the master timeline and every playable source of the snapshot, present and future, from the export clock instead of the steady clock.
   - For each frame n, the clock is set to n / fps, and the sources are updated until `framesCoverClock()` holds. That means every video has taken the frame due then (its next decoded frame is due later, it is at its end, or it is paused) and shows it on its texture (no zero-copy set-up pending).
   - A video that misses the frame within 5 s is drawn with what it has, and the log notes it. Media still opening after 30 s is drawn black.
   - At the start: seek everything to 0, then play the master timeline and every video not locked to it, including one that was paused, stopped or finished when the show was saved. The export is the show running from its start.
   - A video that misses its frame on 3 frames is then waited for at most 50 ms a frame, so a broken file cannot stall the export for minutes.
4. **The live output path draws the frames.**
   - Each app frame, while an export runs, the job takes the place of the live `renderProjectorFrames`. It renders as many frames as fit in about 40 ms, so the editor stays responsive.
   - `OutputManager::setExportRendering(true)` surrounds `renderOutputFrames`. With it on, every frame is drawn at the output's own resolution (rounded down to even, `exportFrameSize`), with no blackout and no Identify.
   - Each chosen output's frame is read back with `glReadPixels`, bottom-up, and handed to an `AsyncMp4Writer`. The projectors show the frames being exported.
   - An output that is removed, or whose resolution changes, fails the export.
   - Blackout still holds on the projectors: `presentOutputs` shows black in blackout whatever the frame holds, because an export's frames are drawn without it.
5. **Encoding** (`video/Mp4Writer`):
   - Encoders are tried in order: h264_mf (Media Foundation, hardware when it opens, else software), then libopenh264, then MPEG-4 Part 2.
   - Bitrate: max(2 Mb/s, 0.12 bits per pixel per frame). One keyframe a second.
   - Colour: BT.709 limited range from 720p up, BT.601 below. MP4 with faststart.
   - The file is written as `.part` and renamed when finished. Cancel, a failure or quitting deletes the `.part`.
   - All FFmpeg calls run on the writer's own thread, with a bounded queue of 4 frames.
6. **Sound** (`audio/OfflineAudioMix`):
   - Each output's file carries the sound of the videos in the scene it shows. Each video plays at its volume (0 when muted), times the master volume (0 when muted).
   - The sound is mixed offline from the files, by the live rule: heard only at 1x going forward, Once is silent after its end, and Loop wraps with the 5 ms crossfade.
   - The samples for frame n are [round(n·sr/fps), round((n+1)·sr/fps)), so the sound never drifts from the picture.
   - A file without sound gets no audio track.
7. **Test hook:** `--stress ... --export-check=<folder>` exports 2 s of the first output after the stress cycles, and fails the run unless the file is written.

## Consequences
- An export is deterministic: every frame is rendered, at any machine speed. A 1080p30 export of a 1080p clip ran faster than real time on the dev PC (60 frames in about 0.5 s).
- The outputs' scene routing is read live: a scene take during the export changes that output's picture in the file, while its sound stays with the scene it started with. Take scenes before exporting, not during.
- If one file of several fails to finish, the files already finished stay and are listed.
- Variable-speed or reverse sound is silent, as it is live.
- Generators step by one frame (1 / fps) per exported frame. A camera shows whatever it sees while the export runs.
- `quadmapper_audio` (OfflineAudioMix) calls `mapTimeline` from `quadmapper_video`. Targets that use it link both, as the app and `test_audio` do.
