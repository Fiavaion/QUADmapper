# ADR-061: Compositions: a timeline and a switcher of sources, on a quad

**Date:** 2026-11-23
**Status:** Accepted (owner request, 2026-11-23; built while the owner was away, for review)
**Deciders:** Jones
**Relates:** ADR-040 (sources), ADR-046 (sources table), ADR-048 (external control), ADR-050 (audio), ADR-060 (video export)

## Context
- The owner asked for two things:
  - "convert video into a composition/timeline with switches and blends, controllable by OSC/MIDI, and applied to a quad";
  - "a multi-layer node that switches videos over OSC".
- Both are one idea: a source made of other sources, which changes between them with a crossfade. Sometimes the change comes on a timer and sometimes on a command.

## Decision
1. **One source kind, two modes** (`source/CompositionSource`):
   - A composition is a list of **cues**. Each cue is a source (a video, an image or a generator) with a hold time.
   - **Timeline:** each cue holds for its time, then the next one crossfades in. The composition loops, or holds on the last cue. A hold of 0 waits for Next.
   - **Switcher:** a cue changes only on Next, Previous or Cue n.
   - Common settings: the crossfade time (0 is a cut) and the picture size (1920x1080 at first; cues are stretched to it).
   - **Restart clips on cue** (on at first): a video cue starts from its beginning when it comes on, and pauses once it has faded out. Off: every clip keeps running and a switch only changes what shows.
   - A composition is never a cue (no nesting).
   - The cue logic is `CompositionPlayhead`, GL-free and unit-tested. Time is driven by `updateAll`'s dt, so a video export steps it frame by frame.
2. **Drawn on the GPU, after the other sources.**
   - `SourceManager::updateAll` skips compositions in its first pass. It then draws each one, in creation order, from the textures its cues took this frame: one fullscreen triangle mixing the cue going out with the one coming in.
   - The output is the composition's own framebuffer, in the same orientation as every other source texture. The texture is wrapped like any GPU-native source, so quads, slices, 3D objects and the export use it unchanged.
   - GL state is saved and restored.
3. **Kept by the editor.**
   - A composition lives until it is deleted in the Compositions tab, whether a quad uses it or not.
   - Its cues' sources are kept alive by it (`releaseUnusedSources`).
   - A project load, New or Restore releases compositions with the rest of the show.
4. **External control** (`editor/ControlBindings.hpp`), on the new `composition` collection, by name or id like quads and sources:
   - `/qm/composition/<name>/next`, `/prev`, `/play`, `/pause` and `/restart` are triggers.
   - `/qm/composition/<name>/cue i <n>` takes the 1-based cue. Feedback sends the cue that is showing.
   - `/qm/composition/<name>/fade f <seconds>` sets the crossfade time.
   - Every target can be MIDI-learned. Saved MIDI mappings are remapped onto the reloaded compositions' ids.
5. **Sound:** a composition heard in a scene passes its gain on to the cue showing, times how far it has faded in, and to the cue going out, times the rest (`addCompositionCueGains`). The Video Player's "Silent" label follows the same rule.
6. **.qmap 3.7:**
   - The new source kind `"composition"` carries `name`, `mode` (`"timeline"` | `"switch"`), `fadeSeconds`, `loop`, `restartOnCue`, `width`, `height` and `cues: [{source, hold}]`.
   - Every composition is written, first and in order, followed by its cues' sources.
   - A cue whose file is missing keeps its missing-source id, so it is written back and listed to relocate.
   - A 3.6 reader skips the unknown kind, and quads showing a composition fall back to no media.
7. **UI:** the **Compositions** tab of the Resources window:
   - New Timeline and New Switcher; the list of compositions; Delete (the quads showing it go to no media).
   - The transport: Previous, Next, Play/Pause, Restart and one button per cue, which work under the perform lock.
   - Settings, and the cue list: hold time, reorder and remove. Add a cue from the media bin list, or drop a bin item on it.
   - **Put on N selected quads.**

## Consequences
- A video used as a cue and also put straight on a quad is one source. With Restart clips on cue, the composition pauses and restarts it, and the quad shows that too.
- A missing cue file is listed in the Missing Media window (as "N cues"), and relocating it re-points the cue (`relocateMissingSource`).
- Removing or moving cues so that a different cue sits under the playhead starts that cue. Turning Restart clips on cue off lets the clips it paused run again; deleting a composition does the same.
- A video added as a cue before its file has opened takes the clip's length as its hold once it is known.
- A video export draws compositions exactly (they step with the export clock), but a composition's sound is not in exported files yet: OfflineAudioMix mixes whole clips, not cue envelopes.
- The panel's edits are not undo steps, like the Inspector's media binding.
