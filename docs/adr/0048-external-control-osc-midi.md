# ADR-048: External control — OSC and MIDI with learn, over a small control-target registry

**Date:** 2026-09-26
**Status:** Accepted (owner approved PRD steps 1–4, 2026-09-26)
**Deciders:** Jones

## Context

The PRD lists MIDI control (US-102, P1) and OSC as the first post-MVP features; the mvp spec has "MIDI input" and "OSC input/output" for v1.1. The foundation audit proposed a full parameter registry (every value typed and addressable, with the UI, undo and serializer bound to it). That is the right long-term shape, but it would rewrite every panel. Only external control needs addressing today.

Facts that shape the design (research, 2026-09-26):
- Quads, groups, scenes, sources and outputs have UUIDs that survive save/load. The serializer creates fresh ones only for legacy files that lack them. Names are not unique.
- `KeyboardShortcuts`/`InputManager` is dead code (the per-frame call is commented out). There is no action registry.
- `src/core` has no thread-safe queue. WinMM MIDI callbacks and a UDP receive loop both run off the main thread.
- There is no blackout.
- App preferences are JSON (`AppPreferences`). `src/ai` already links a Windows system library (`winhttp`) with no vcpkg dependency, which sets the precedent.

## Decision

### 1. A new `src/control` module (depends on `core` only)
- `MidiInput` uses WinMM (`midiInGetNumDevs`, `midiInOpen` with `CALLBACK_FUNCTION`). It can open any number of devices by name and decodes note on/off, CC and program change. SysEx is ignored.
- `OscServer` is a Winsock UDP socket on its own thread. It includes a hand-written OSC 1.0 parser covering messages and bundles, argument types `i f s T F`, with every length and bound checked. Malformed packets are dropped and counted, never trusted.
- Both push `ControlEvent`s into a mutex-guarded `ControlEventQueue`. The main thread drains it once per frame, before UI, in the `processPendingFileAction` slot. No scene state is touched off the main thread.
- New system libraries: `winmm` and `ws2_32`, linked `PRIVATE` in `src/control/CMakeLists.txt`. There is no third-party dependency.

### 2. The control-target registry (in `app`, rebuilt when the project changes)
A target is `{path, kind: Trigger | Float | Bool, range, apply(value)}`, where `apply` runs on the main thread. Paths:

| Path | Kind | Effect |
|---|---|---|
| `/qm/scene/take` `i n` | Trigger | Takes the nth scene (1-based), the same code path as hotkeys 1–9 |
| `/qm/scene/{id or name}/take` | Trigger | Takes that scene |
| `/qm/quad/{id or name}/opacity` | Float 0–1 | |
| `/qm/quad/{id or name}/visible` | Bool | |
| `/qm/source/{id or name}/play`, `/pause`, `/restart` | Trigger | |
| `/qm/source/{id or name}/speed` | Float 0.1–4 | |
| `/qm/source/{id or name}/volume` | Float 0–1 | Only once audio exists (ADR-050) |
| `/qm/master/volume`, `/qm/master/mute` | Float, Bool | Only once audio exists |
| `/qm/blackout` | Bool (a Trigger toggles) | |

- **Name resolution:** the UUID string is tried first, then the case-insensitive name. An ambiguous name uses the first match in list order and logs one warning per address. Scene and quad names are looked up in the active project only.
- **Live values:** external control changes state directly, without undo entries (a fader sweep would flood undo). The project becomes dirty through the existing content fingerprint, so a saved show keeps the current values, as in other VJ tools.

### 3. Blackout
A master blackout flag on the output manager. When it is on, every output renders black and the editor canvas shows an unmistakable "BLACKOUT" banner. It is toggled from the Output menu, the control targets, and a shortcut chosen to avoid the canvas's editing keys. It is runtime state and is not saved.

### 4. MIDI learn and mappings
- A `MidiMapping` is `{device name or "any", type: note | cc | program, channel 1–16 or any, number, target path, mode}`, where mode is Trigger, Toggle, Momentary or Value.
  - Trigger fires on note-on with velocity > 0, or on a CC crossing 64 upward.
  - Value maps CC 0–127 linearly into the target's range. It uses 7-bit precision; 14-bit CC is not supported in v1.
- **Learn:** a Control panel lists every current target. Clicking "Learn" on a row and then moving a control binds the next incoming note or CC. Esc cancels. Mappings can be deleted and show their last activity.
- **Where things are saved:**
  - Mappings are show content and are saved in the project as `.qmap` v3.3 `"control": {"midi":[…]}`. The change is additive: older builds ignore it, and a missing field means no mappings.
  - Device selection, the OSC enable flag, the port (default 8000) and the bind address (default all interfaces) are app preferences.
- OSC needs no mapping: a message's address is the target path. The panel shows the last OSC messages received, so a user can see the controller is reaching the app.

### 5. Safety
- OSC is **off by default**. Enabling it opens a UDP port; Windows Firewall will ask the first time, which the Preferences dialog explains.
- MIDI or OSC can never change geometry, load or save files, or quit the app.
- Unknown addresses are counted and shown, never fatal.
- Queue draining is capped per frame, so a flood cannot stall rendering.

## Consequences
- ✅ MIDI with learn and OSC input are covered with no third-party dependency. Show mappings travel with the project.
- ✅ The registry is small and grows by adding rows. The full ADR-048-style parameter registry (UI, undo and serializer bound to typed parameters) can absorb it later without changing the addresses.
- ✅ Blackout gives live control a panic button.
- ❌ OSC is input only; feedback and output are v2. There is no 14-bit MIDI and no MIDI clock or timecode. (Feedback, address patterns and 14-bit CC were added by Amendment 1; clock and timecode remain.)
- ❌ External changes are not undoable (deliberate).
- ❌ Name addressing is ambiguous when names repeat; UUID addressing is always exact.

## Amendment 1 (2026-09-26): OSC feedback, address patterns, 14-bit CC (TD-25)

Added within the same module and the same socket; no new dependency, no format break.

- **OSC feedback (output).** `OscWriter` writes OSC 1.0 messages (`i f s T F`), tested byte-exact against the specification's examples and round-tripped through the parser. `OscFeedback` decides what to send, with injected time: on change only, at most one message per address per 1/30 s, coalesced to the latest value, at most 256 per update, round robin. `ExternalControl::updateFeedback` samples every Float, Bool or Index target that has a reader and sends from the OSC input socket (`OscServer::sendTo`), so feedback needs OSC input on. Float and Bool go as `f` (Bool as 0/1), Index as `i`.
  - Addresses: `/qm/blackout`, `/qm/master/volume`, `/qm/master/mute`, `/qm/timeline/running` (new Bool target: play/pause, and its light), `/qm/scene/active i n` (new, OSC only: the active scene; setting it takes that scene), `/qm/quad/{q}/opacity`, `/qm/quad/{q}/visible`, `/qm/source/{s}/speed`, `/qm/source/{s}/volume`, `/qm/source/{s}/mute`. An entity is addressed by its name when the name is unique in its collection and a valid OSC address part (so a controller that sends by name is answered on the same address), otherwise by UUID; "Address by id" forces UUIDs.
  - Destination: reply to the last sender's IP on a configured port (default 9000; 0 = the sender's own port), or a fixed dotted-IPv4 host (no DNS lookup). A new destination (a different controller, changed settings, OSC restarted) receives every value once; the Control panel has "Send all", and a controller can send the trigger `/qm/feedback/refresh` to get every value again (a restarted controller asking for state). A value a controller has just sent is not echoed back to it (a value the target corrected, such as a clamped one, is).
  - Settings are four `ControlPreferences` fields (`oscFeedbackEnabled` default off, `oscFeedbackHost`, `oscFeedbackPort`, `oscFeedbackUseIds`); the panel shows status, destination, packets sent and send errors.
- **Address patterns.** Incoming addresses that do not resolve literally are expanded as OSC 1.0 patterns (`? * [abc] [!a-z] {foo,bar}`, part by part, without case) against the registered targets; an entity part matches a name, and matches a UUID only when spelled out (literal characters and `{a,b}` choices, no `? * []`), so `F*` never picks an entity whose random id starts with f (fixed 2026-09-27; it made a take pattern flaky). The matcher simulates sets of positions, so its cost is polynomial (pattern length × text length); patterns over 256 characters or malformed ones match nothing and count as unknown addresses, as does a pattern with no match. A pattern may fire a trigger on several sources, but one that matches more than one scene take (entity takes, `/qm/scene/take`, `/qm/scene/active`: targets marked `exclusive`) takes none of them and is counted and shown as a refusal; the other targets it matched still act.
- **14-bit MIDI CC.** A mapping may be `"bits": 14` (CC 0–31 as MSB with n + 32 as LSB). Learn upgrades a CC to 14-bit when the other half of the pair (either order) follows within 0.1 s on the same device and channel. Value mode then uses 0–16383; press/release edges come from the MSB. The MSB resets the LSB to 0 (MIDI 1.0) unless an LSB arrived since the last MSB within the window, so LSB-first controllers also land on the exact value. `.qmap` stays 3.3.0: `"bits"` is written only for 14 and a missing field means 7. NRPN/RPN are out of scope.
- **Not done:** MIDI Timecode chase (needs WinMM SysEx buffers for full-frame messages and a seekable master timeline in the concurrently changing playback code) and MIDI clock; OSC bundle time tags are still ignored. These stay in TD-25.

## Alternatives considered
1. **The full parameter registry first.** Rejected for now: it rewrites every panel to deliver a feature that needs about a dozen targets.
2. **RtMidi / liblo / oscpack via vcpkg.** Rejected: WinMM and Winsock cover what is needed in a few hundred lines, and every dependency needs an ADR plus upkeep.
3. **Reviving `KeyboardShortcuts` as the action layer.** Rejected: dead, keyboard-shaped, and slated for replacement by the ADR-051 action registry.
