# ADR-046: Show-safe persistence: atomic save, recovery, and a `.qmap` v3.1 sources table

**Date:** 2026-09-25
**Status:** Accepted
**Deciders:** Jones

## Context

The [2026-09-25 foundation audit](../audits/2026-09-25-foundation-audit.md) (Tier 0, items 0.3–0.6) found that the project file is the least show-safe part of the app:

- `FileSystem::writeFile` truncates the target and writes in place, with no temp file, rename or backup. A crash, power loss or full disk during a save leaves an empty or half-written `.qmap`.
- `newProject` never resets `EditorState::projectPath`, so File > New followed by Ctrl+S overwrites the previous project.
- There is no unsaved-changes prompt on New, Load or Exit. The `autoSaveEnabled` preference is never read. No exception guard sits above `Application::run()`.
- Only a per-quad `texturePath` is saved. `TextureBinding::mediaId` never is, so a procedural source binding (which has an empty path) disappears on save/load. Transport state (playing, loop, speed) is also lost. `Project.hpp:68` promises relative media paths, but `makeRelative` has no callers.
- `deserializeVec2`/`Vec3` index JSON arrays without size or type checks, which is UB in nlohmann. Scene, group and output IDs use the throwing `UUID::fromString`.
- File > Load runs from a menu callback in the middle of `Application::render()`. It destroys the Scene that owns a `QuadManager&` which the rest of that frame still uses.

The `.qmap` format is a Change Boundary. Anything we write now has to keep loading forever, which is why this needs an ADR.

## Decision

### 1. Atomic writes
Add `FileSystem::writeFileAtomic(path, content)`, which:
1. writes `<path>.tmp` in the same directory;
2. flushes it and checks that the close succeeded;
3. if `<path>` exists, copies it to `<path>.bak` (a single rolling backup);
4. replaces the target with `MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.

On any failure it returns `Err` and leaves the original file untouched. Every project save goes through it, including autosave and emergency saves.

### 2. `.qmap` v3.1.0 (additive minor bump, per ADR-032)
- A new top-level `"sources"` array. Each entry is `{ "id", "kind", ... }`:
  - `kind` is one of `image` | `video` | `hap` | `procedural` | `camera`.
  - File kinds carry `"path"`, written **relative to the project file's directory** when `lexically_relative` succeeds (same root), and absolute otherwise.
  - `procedural` carries `"proceduralType"` (the `ProceduralFactory::getTypeName` string), plus `"width"` and `"height"`.
  - `camera` carries `"deviceName"`.
  - `video` and `hap` carry an optional `"transport": { "playing", "loopMode", "speed" }`.
- A quad's `texture` object gains `"mediaId"`, which references `sources[].id`.
- **Back-compat:**
  - `texturePath` is **still written**, as an absolute path, so a v3.0 build still opens a v3.1 file with its images and videos. Unknown fields are ignored under the ADR-032 policy.
  - On load, `mediaId` → `sources` takes precedence. If it is missing or unresolvable, the loader falls back to `texturePath`, which is how every v1, v2 and v3.0 file loads.
  - A relative `path` resolves against the project directory. If that file doesn't exist, the loader falls back to the absolute `texturePath`.
- A source's `id` is persisted as a stable key within the file. At runtime `SourceManager` may mint new IDs, and the loader remaps them (the same pattern as the existing quad, group and output remaps).
- The writer emits `"version": "3.1.0"`. The reader accepts any major ≤ 3, as today.

### 3. Input hardening (no format change)
- Checked `Vec2`/`Vec3` helpers that reject arrays of the wrong size or type and return `Err`.
- Tolerant UUID parsing for scenes, groups and outputs (the `parseIdOrNull` pattern).
- Grid coordinates sorted, deduplicated, NaN-rejected and clamped to the existing maximums.
- `meshMode` range-checked.
- Output `monitor`, `resolution` and `rotation` clamped to sane ranges.

A malformed file returns `Err` and never crashes the app.

### 4. Session safety in the app
- **Deferred file actions.** File > New, Load, Open and Exit set a pending action on `EditorState`. `Application` runs it at a frame boundary, before `render()` takes any references. This removes the dangling `QuadManager&`.
- **Undo history.** `commandHistory.clear()` is called explicitly on New and Load.
- **New project.** New resets `projectPath` to empty (untitled). It also resets the scenes to one default scene, clears `SourceManager`, keeps the outputs, and clears the modified flag. Saving an untitled project asks for a path through the native Save dialog, and the dialog is run from the frame-boundary action.
- **Dirty prompt.** New, Load, Exit and window close (`glfwSetWindowCloseCallback`) show a modal when the project is modified: *Save / Don't Save / Cancel*.
- **Autosave / recovery.**
  - When `autoSaveEnabled` is on (the default) and the project is modified, the app writes `<project>.autosave.qmap` every `autoSaveInterval` seconds (default 60). An untitled project autosaves to `%LOCALAPPDATA%\QUADmapper\recovery\untitled.autosave.qmap`.
  - The write is atomic, and a successful manual save deletes the autosave file.
  - At startup, if an autosave is newer than its project (or an untitled one exists), a modal offers *Restore / Discard*.
- **Crash guard.** `main()` wraps `run()` in `try/catch(...)`. On an exception it logs and makes a best-effort emergency save to `<project>.crash.qmap` (or the recovery directory), and never overwrites the real file.

## Consequences
- ✅ No save path can destroy the last good project, and a crash costs at most one autosave interval.
- ✅ Procedural, camera and HAP bindings and video transport round-trip. Shows are portable when the media sits beside the project.
- ✅ Older builds still open new files; they just drop procedural bindings, as they do today.
- ❌ The file grows by a `sources` table plus a duplicated `texturePath` per quad. That duplication stays until a major bump retires `texturePath`.
- ❌ `<project>.bak`, `.autosave.qmap` and `.crash.qmap` files appear next to the project. This is documented, and the same convention pro tools use.
- ⚠️ Stable quad, group and output IDs across load are **not** part of this ADR. They belong to the parameter/addressing ADR (Phase 1).

## Alternatives considered
1. **Replace `texturePath` with `mediaId` outright in a v4.0 major bump.** Rejected: v3 builds could no longer open new files, and there is no user benefit yet.
2. **Store sources implicitly, by writing the procedural type into the quad's texture object.** Rejected: that duplicates source state per quad, breaks the one-source-many-quads model (ADR-040), and gives future live inputs (NDI, Spout) nowhere to live.
3. **`ReplaceFileW` instead of `MoveFileExW`.** Rejected: it fails when the target doesn't exist yet (the first save) and needs a separate code path. Copy-to-`.bak` plus `MoveFileExW` covers both cases.
4. **Autosave into the real project file.** Rejected: it silently commits experimental edits, and technicians expect "Save" to mean something.

## Amendment (QM-087): monitor identity
- Each output in `outputs` gains two optional, additive fields next to `monitor`: `monitorName` (the GLFW monitor name, a string) and `monitorPosition` (`[x, y]`, the monitor's origin on the virtual desktop). They are written only when the output has a connected monitor chosen. No version bump: older builds ignore them under the ADR-032 policy, and files without them load as before.
- On load, the saved monitor is resolved against the connected displays by `OutputManager::resolveMonitor`, in this order: name + position, then name (preferring the saved index when several displays share a name, as identical projectors do), then the saved `monitor` index. If none matches, the output goes to monitor 0 and a warning is logged. An output that never had a monitor (`monitor` -1, no name) stays unassigned.
- `monitor` is still written, so a build without this amendment still finds its screen by index.

## Amendment (QM-090): desired vs resolved monitor
QM-087 resolved the saved monitor once, at load, and fell back to monitor 0. On real machines every display is often "Generic PnP Monitor", and monitor 0 is usually the operator's own screen. A show opened before the projector was plugged in therefore went fullscreen over the editor at F11, and the next Save overwrote the projector's identity with the laptop's. This amendment replaces the QM-087 resolution rule.
- **Two identities per output.** Each output keeps a *desired* monitor `{name, position, savedIndex}`, taken from the `.qmap` or from an explicit choice in the Output panel or Inspector (which records that monitor's current identity). The *resolved* monitor is whichever connected display that identity maps to right now, or none.
- **Save writes the desired identity only** (`monitor`, `monitorName`, `monitorPosition`), never a fallback. A fallback therefore does not change the file, and does not mark the project dirty. An output with no chosen monitor writes `monitor: -1` and no name.
- **Resolution order** (`OutputManager::resolveMonitor`, pure and unit-tested): name + position → position only → name only, if no other connected display shares it → the saved index → the first connected display → unresolved. Every step after name + position skips the editor window's display and displays already taken by other outputs. Stronger matches claim displays first, across all outputs. A name + position match is honoured even when it is the editor's display, since only an explicit choice records that identity.
- **When.** Monitors are resolved at load (for the Output panel), again at F11 after the monitor list is refreshed, again on every monitor hotplug, and after an explicit choice. In perform mode an output moves when its display changes, and it is hidden when it is left without one.
- **Unresolved at F11.** The output is not shown, and a status message says so ("Projector 'Output 1' not connected; its output is not shown"). The other outputs still enter perform mode. With only the editor's display connected, F11 covers it only if the user chose that display.
- **Load hardening.** A missing or malformed `monitorPosition` drops the monitor name with a warning instead of failing the load. A name without its position would match whatever display sits at (0, 0).
