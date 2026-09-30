# ADR-051: Editor architecture: an editor library, one Panel interface, an action registry, a perform lock

**Date:** 2026-09-28
**Status:** Accepted (owner: "continue", 2026-09-28). Reserved by the foundation audit, 2026-09-25.
**Deciders:** Jones

## Context

- **`Application.cpp` is 3,019 lines** (the audit's target is under 1,200). It holds:
  - the live⇄DTO project mapping (about 450 lines, untested);
  - file actions, autosave and recovery (about 650);
  - the AI One-Flash code (about 260);
  - a 660-line `render()` with a 175-line shortcut block;
  - the dialogs' ImGui code.
- **`ui` and `app` include each other.**
  - `EditorState` holds 11 panels by value, and panels include about 40 `app/` headers.
  - Editor logic reaches into panel state: the media bin's selection and preview source, the output selection.
  - `test_control_app` links the whole `ui` library only because `EditorState` drags the panels in.
  - The editor helpers are header-only in `src/app` because the app is an executable (TD-14).
- **Panels are wired by hand.** Each draws its own window from an `EditorState::show*` flag. Two are set up lazily through setters, and the menu bar takes 8 AI callbacks. The Window menu lists hard-coded flags.
- **Shortcuts are hard-coded** in `render()`. The menu disagrees with the keys:
  - "Deselect All — Escape" does nothing;
  - menu Delete ignores objects and calibration points;
  - menu Select All ignores the calibration guard.

  Modifiers match loosely: Ctrl+Shift+S also saves.
- **Nothing stops an accidental edit during a live show.**
- `KeyboardShortcuts`/`InputManager`, named by the audit, were already deleted (2f3ae37). Dead stubs remain:
  - `renderer/{Mesh,VertexArray,VertexBuffer,RenderContext,Shader}`;
  - `output/PerformMode`, `ui/menus/MainMenu`, three widgets, `OutputConfigDialog`;
  - the never-instantiated `ProceduralEditorPanel`;
  - the unused mesh shaders;
  - `Application::update()` and `showDemo`.

## Decision

### 1. Layers
`core → {scene, project, media, video, audio, control, renderer, source, output, input} → editor → ui → app`

- **`src/editor`, a static library (`quadmapper_editor`).**
  - **Contents:**
    - `EditorState` (with no panels) and `EditorContext`;
    - the commands helpers and mutations now in `src/app`: GridUndo, MediaAssign, MeshEditKeys, MeshLoads, MissingMedia, OutputPersistence, ProjectReplace, QuadSelection, ReplaceMedia, SliceEditing, SourceAudio, TextureResolve, UndoRouting, UnsavedIndicator, ControlBindings;
    - the live⇄DTO mapping as `ProjectMapping.cpp`;
    - the action registry and the perform lock;
    - `EditorServices`, a pure interface the app implements: file-action requests, status, missing-media search, dialog state, preference saves.
  - **ImGui:** it has no ImGui dependency beyond `input/VertexKeyboard.hpp`, which stays put so the success-metric code does not move.
- **Model state leaves the panels.** The selected media id, the selected output id and the preview source move into `EditorState`. Editor code never touches a panel.
- **`ui`** holds the panels, `EditorUi` (dock layout, menu, canvas overlay, banners, dialogs), `PanelHost` and `ShortcutRouter`.
- **`app`** holds:
  - `Application`: the loop, GLFW/GL/ImGui setup and shutdown, projector frames;
  - `ProjectSession`: New/Open/Save, autosave, recovery, dirty state, emergency save;
  - `AiAssist`: the sidecar and One-Flash;
  - `AppActions`: registers the actions that need app services;
  - `Bench` and `StressHarness`.
- **The layering is tested.** A `layering_check` ctest fails on any `ui/` or `app/` include under `src/editor` and any `app/` include under `src/ui`. A `test_editor` target links only the editor library.

### 2. One Panel interface
```cpp
enum class PanelKind { Window, Tab, Overlay };           // Tab = inside Resources
class Panel {
public:
    virtual ~Panel() = default;
    virtual const char* id() const = 0;                  // window, dock and imgui.ini name
    virtual const char* menuLabel() const { return id(); } // Window menu; nullptr = unlisted
    virtual PanelKind kind() const { return PanelKind::Window; }
    virtual bool editsShow() const { return true; }      // drawn disabled under the perform lock
    virtual void onAttach(EditorContext&) {}             // replaces lazy setters and callbacks
    virtual void onProjectReplaced(EditorContext&) {}    // New / Open / Restore
    virtual void drawContent(EditorContext&) = 0;        // the host does Begin/End
    bool visible = true;
};
```
- The host opens the window, applies `BeginDisabled` when the lock is on and the panel edits the show, and closes it.
- The Window menu is generated from the host's panels.
- Editor code that must show a window posts a request, which the host drains.
- Panels get everything through `EditorContext`: state, project, sources, outputs, renderer, audio, actions, services, external control, preferences.
- `ViewportEditor` and `MainMenuBar` stay non-Panel classes owned by `EditorUi`, for ordering (TD-21: the canvas runs straight after `NewFrame`) and because the menu bar is not a window.

### 3. Action registry
```cpp
struct Chord { int key; uint8_t mods; };                 // ImGuiKey, exact Ctrl|Shift|Alt|Super
enum class ActionClass { Show, Edit, File, View };
enum class KeyGuard { Editing /*no text field, no popup*/, Show /*no text field*/ };
struct Action { std::string id, label; std::optional<Chord> chord; ActionClass cls;
                KeyGuard guard = KeyGuard::Editing; bool fixed = false;
                std::function<bool(const EditorContext&)> enabled;
                std::function<void(EditorContext&)> run; };
enum class Outcome { Ran, Disabled, Locked, Unknown };
```
- **`add()` refuses:**
  - duplicate ids;
  - clashing chords;
  - any chord on a reserved canvas key (Tab, the arrows and H, with any modifiers).
- **The menu and the keys invoke the same action**, and the menu shows the registry's label and chord, so they cannot disagree. Modifiers match exactly.
- **Canvas keys stay outside the registry:**
  - Tab/Shift+Tab, the arrows with Shift 10× and Alt 0.1×, and H/Shift+H;
  - widget-local Esc.

  They are modal, they repeat, and Shift/Alt are step sizes, not chord parts. `SetKeyOwner` keeps protecting them. They are the success metric.
- **Captured outside, dispatched through the registry:**
  - scene takes 1–9 and numpad keys, from the physical GLFW key (layout-independent) through the take queue;
  - an output window's own Esc/F11/Ctrl+B/1–9, which stay in `OutputManager` because output must not depend on editor.
- **In the registry:**
  - with the looser `Show` guard and not rebindable: F11, Esc (the perform/calibration/learn chain) and Ctrl+B;
  - with the `Editing` guard: Delete, Ctrl+Z/Y/A/,/S, Ctrl+[/] (±Shift), Ctrl+M, Alt+=/−/R.
- **No new bindings.** New menu chords such as Ctrl+N/Ctrl+O/Ctrl+Shift+S need the owner's sign-off first. Rebinding and a keymap file are deferred. *(Superseded by the 2026-09-30 amendment below: the chords are approved, an action has a list of chords, and the user can rebind them.)*
- **OSC/MIDI (ADR-048) keeps its own `ControlRegistry`.** Only `Show`-class actions may ever be exported to external control.

### 4. Perform lock
- **Blocks:**
  - `Edit` actions;
  - canvas picking, dragging and keys (no canvas key owner);
  - media drops;
  - show-editing panels, which are drawn disabled;
  - New, Open, Import OBJ and Restore;
  - calibration and slice-warp editing;
  - One-Flash;
  - MIDI learn and mapping edits;
  - Preferences edits;
  - as a safety net, `CommandHistory` refuses commands.
- **Keeps:**
  - scene takes by key, OSC and MIDI, with their fades;
  - blackout, F11/Esc and perform mode;
  - every OSC/MIDI target;
  - the Video Player transport;
  - Save and autosave;
  - canvas zoom, pan and viewing;
  - Exit, which still confirms.
- **Toggle:**
  - Output > Perform Lock, and `perform.lock` on Ctrl+Shift+L, which is currently unused (a new binding: listed for the owner's sign-off before E7 ships; approved 2026-09-30, see the amendment).
  - Locking is immediate. Unlocking asks for confirmation.
  - A persistent LOCKED banner shows while it is on.
- **Persistence:** runtime state, like blackout. It is not saved, and there is no `.qmap` change.

### 5. Migration, in slices that each build and pass the full ctest
- **E0.** Delete the dead stubs, `ProceduralEditorPanel` and the mesh shaders.
- **E1.** `AiAssist` out of `Application.cpp`; delete `update()`/`showDemo`.
- **E2.** `ProjectSession` out (file actions, recovery, autosave, dirty state, missing-media search, emergency save).
- **E3.** Break the cycle: panels out of `EditorState`, selection/preview state into it, editor helpers stop touching panels.
- **E4.** Create the `quadmapper_editor` library (`git mv`), `ProjectMapping.cpp` with a round-trip test, `test_editor`, `layering_check`.
- **E5.** `Panel`, `PanelHost`, `EditorUi`: `render()`'s layout, panels, banners and dialogs move to ui; visibility moves into panels; the Window menu is generated.
- **E6.** `ActionRegistry`, `ShortcutRouter`, `AppActions`; the shortcut block is deleted; the menu invokes actions.
- **E7.** The perform lock.
- **E8.** CLAUDE.md (component graph, "Adding a New Panel", "Adding a New Keyboard Shortcut", Key Files) and the debt ledger.

E0 can run alongside anything. E1, E2, E3 and E5 all touch `Application.cpp`, so they merge in order. E4 runs alone because it renames headers.

**E4 as built (2026-09-28).** Where it differs from the plan above:
- `StatusLine.hpp` moved to `src/editor` too; `MeshLoads` needs it.
- **Preferences (D2):** the `AppPreferences` model and its file I/O are now in `editor/Preferences.{hpp,cpp}`. `ui/dialogs/PreferencesDialog` keeps the window and the config path. `ui` no longer links nlohmann_json.
- **Test targets:**
  - `test_editor` links `quadmapper_editor` alone.
  - `test_ui` is new: the Preferences window and the calibration view on the canvas, split out of `ControlBindingsTest` and `SceneTakeRoutingTest`.
  - `test_control_app` is renamed `test_app` and holds the project session only.
  - `test_ai_assist` links the editor instead of `ui`.
- **`layering_check`** is `tests/LayeringCheck.cmake`. It also catches relative includes (`"../ui/…"`).
- **Still to do:**
  - Three lower-module tests include editor headers header-only: `OutputManagerTest` (`OutputPersistence`), `ProjectTest` (`UnsavedIndicator`) and `AudioSyncTest` (`SourceAudio`).
  - CLAUDE.md and `.claude/rules/scene-geometry.md` still name `src/app` paths for `EditorState` and `MeshEditKeys`. They are left for E8.

**E7 as built (2026-09-29).** Where it differs from §4 or adds to it:
- **State:** `editor/PerformLock.{hpp,cpp}`, a `PerformLock` member of `EditorState` (runtime only, no `.qmap` change). `lockPerform`, `requestPerformUnlock`, `confirmPerformUnlock` and `cancelPerformUnlock` are the only ways to change it, so an unlock cannot skip its confirmation.
- **Toggle:** `perform.lock` (Show class) on Output > Perform Lock. **No chord yet:** Ctrl+Shift+L waits for the owner's sign-off; `makeAppAction` has a comment where it goes. The unlock confirmation is the "Unlock the Show?" modal in `SessionDialogs` (Esc = Stay Locked). The LOCKED badge sits in the canvas's top-right corner (`Banners`), clear of the zoom controls and the BLACKOUT banner.
- **Calibration and slice-warp editing end on lock** (the safer of the two options: the lock always engages at once, and no editing mode stays active behind it). Every drag in progress (a warp corner, a quad corner, a Bezier handle, a mesh vertex, a grid handle, a division being inserted) is kept where the mouse left it as its undo step before the history turns read-only. `beginCalibration` and `beginSliceWarp` refuse while locked.
- **Policy:** the registry refuses Edit-class actions and posts "<label> is blocked: the show is locked (Output > Perform Lock)"; the menu greys them out. File-class Save and View-class Preferences (which opens a disabled window) stay allowed.
- **File actions:** `ProjectSession` refuses New, Open, Load, Import OBJ and Restore before the unsaved-changes prompt. Save As, Exit and Discard recovery stay allowed.
- **Panels:** `editsShow()` is false for Resources (the tab bar only: each tab decides), Preview, Video Player, Banners and the session dialogs. Video Player disables everything but its transport (Play / Pause / Stop / ±5 s / Progress): loading, the path, Lock to master timeline, speed, loop mode, per-clip sound, Assign / Remove and Unload. Control stays disabled (MIDI learn and mappings); locking also cancels a pending learn. The Convert to HAP overlay stays disabled, since "Replace in show" edits the show.
- **Kept outside the panels:** the menu bar's master timeline, master audio and output device, Reset Playheads, the Simple / Advanced toggle and "Send to Projector (F11)". The AI menu is greyed except One-Flash > Clear Pattern, which gives the show back; `AiAssist::run` refuses the rest.
- **Safety net (a `core` interface change):** `CommandHistory::setReadOnly(bool)` / `isReadOnly()`. While read-only, `execute`, `undo` and `redo` refuse and log; `clear()` still works. It only catches edits that go through `execute`. Drags, nudges, binds (`bindSourceToQuad`), relinks and OSC targets change the show first and record their step after, or never, so the net cannot stop them. Every such path is closed at its source instead: the canvas, the panels, the menu, the pickers and the other paths listed here.
- **Pickers answered after locking (review fix):** the file dialogs have no owner, so the menu stays usable while one is open. `ProjectSession::objectFileChosen`, `locateFileChosen` and `searchFolderChosen` refuse under the lock with a status. So does the Video Player's `loadVideo` for a pick answered after locking, which would otherwise bind the clip to the selected quad and play it on air. A Missing Media folder search that finishes under the lock is held: `pollMissingMediaSearch` applies it on the first poll after the unlock.
- **An OBJ whose parse finishes under the lock**, including one started before locking, keeps its object (added before the lock was taken) and its mesh, without an undo step, since the read-only history would refuse it (`MeshLoads`). It does not re-aim any projector: re-framing an uncalibrated projector on the new object would move everything on air. The status says so.
- **Esc (review fix):** `perform.escape` acts with a popup open and runs before the session modals, so one Esc on "Unlock the Show?" also left perform mode. `performEscape` (editor/PerformLock) now gives Esc to a session modal alone: the unlock prompt stays locked, Unsaved Changes cancels, and the recovery prompt is left alone. Nothing else happens on that press. F11 under the lock with no outputs refuses to create "Projector 1" (`performToggle`).
- **Scene take keys (review fix):** keys 1–9 pass through the unlock prompt (`runSceneTakeKeys`), since the show stays live while it asks. A take held off by another popup posts a status. A digit typed into a text field is still ignored without one.
- **Blend Test Pattern:** locking turns it off, and the status says so. It is chosen over keeping an Off switch live under the lock because the switch is in the Inspector's slice editor, which the lock disables. The pattern replaces the show on every projector, so an operator who locks means the show to be on air. The lock never turns it on.
- **Not blocked:** files dropped from Explorer still go into the media bin, which is not show content.

**E8 as built (2026-09-29).** CLAUDE.md and the agent and rule files follow the layers. The three lower-module tests that used editor headers moved to `test_editor`: `OutputPersistenceTest` (from `OutputManagerTest`), `UnsavedIndicatorTest` (from `ProjectTest`) and `AudioSyncTest`, whole, whose `test_audio_sync` target is removed. `Application.cpp` is 860 lines. Chords still waiting for the owner's sign-off: Ctrl+Shift+L (perform lock), Ctrl+N / Ctrl+O / Ctrl+Shift+S and Ctrl+Shift+Z (redo).

**Amendment: shortcuts and rebinding (owner's approval, 2026-09-30).** The owner approved the pending chords and asked that "shortcuts can be changed in the app". This supersedes the "No new bindings" bullet of §3, which deferred rebinding and a keymap file.
- **New defaults:**
  - `perform.lock` on Ctrl+Shift+L (Show class, as before).
  - `file.new` Ctrl+N, `file.open` Ctrl+O, `file.saveAs` Ctrl+Shift+S: File-class actions (`registerFileActions`, editor/EditActions) that queue the same `pendingFileAction` as the File menu did. The File menu now invokes them, so it shows their chords.
  - `edit.redo` on Ctrl+Y and Ctrl+Shift+Z.
- **Several chords per action.** `Action::chords` is a list; the first is the one the menu shows (`actionMenuItem`), and the router fires the action on any of them. `add()` checks every chord (key, canvas keys, clashes with other actions and within the list) and records it as `defaultChords`.
- **Lock policy.** `performLockRefuses` refuses the Edit class and, by id, `file.new` and `file.open`. Save and Save As stay allowed; the menu greys New and Open through the same predicate.
- **Rebinding (Edit > Preferences > Keyboard).** Every registered action is listed with its chords. Change captures the next chord with its modifiers (Esc cancels); Add captures a second chord and Remove drops the extra ones; Reset and Reset All restore the defaults. `ActionRegistry::setChords` refuses, with an inline message in the window:
  - the canvas keys (Tab, the arrows, H) with any modifiers;
  - a chord another action has, naming it ("Ctrl+S is already used by Save Project.");
  - a bare key other than F1–F24 and Delete: a bare letter would fire while typing outside a text field, and 1–9 stay the scene takes;
  - `fixed` actions (F11, Esc, Ctrl+B), which are listed as "Fixed" with no buttons.

  While a chord is captured, `EditorState::capturingChord` keeps the router and the scene take keys off the key, and the window asks ImGui for the keyboard so the canvas ignores it. Closing the window, leaving the tab or locking the show ends the capture. The window is drawn disabled under the lock, as before.
- **Storage.** `AppPreferences::keyBindings`, saved as an additive `"keyboard"` object in `preferences.json`: action id → list of chords as text (`{"perform.lock": ["Ctrl+Shift+K"]}`), only for actions off their defaults. A file without it loads unchanged. Entries of the wrong shape are skipped at load; at apply (`applyKeyBindings`, at startup after the actions are registered) an unknown id, a fixed action, an unparseable chord or a chord `setChords` refuses is skipped with a log line and the action keeps its default. Overridden actions let go of their chords before the overrides are applied, so two actions can swap chords. The Keyboard tab changes the registry at once and saves the overrides.
- **Chord text** (`chordText`, `parseChord`) moved from `ui/ShortcutRouter` to `editor/ActionRegistry`, with its own key-name table, so the editor still makes no ImGui calls.
- **Lock tweaks:**
  - The Project tab is no longer drawn disabled as a whole (`editsShow` false): its Save works under the lock, while its path field and Load stay disabled.
  - The Convert to HAP overlay is no longer drawn disabled as a whole: Cancel (and Dismiss) work under the lock; "Replace in show" stays disabled, and its callback refuses under the lock as well.
  - Save As, the audio device and master volume, and the Simple/Advanced toggle stay available, as before.

### 6. Gates
- Every slice keeps `ctest` green, including `bench_smoke`, which drives Tab + arrow nudges and 1–9 through the real GLFW key callback: the automated success-metric gate.
- **Tests:**
  - `ProjectMappingTest` round-trips scenes, quads, groups, missing sources and MIDI mappings.
  - `PanelHostTest` and `ActionRegistryTest`/`ShortcutRouterTest` check that:
    - Delete in a text field does nothing;
    - Ctrl+B fires with a popup open;
    - Tab with a quad selected is never consumed;
    - the menu and the key have the same effect.
  - `PerformLockTest`.
  - `layering_check`.
- **Manual after E5, E6 and E7:** the success metric by hand, F11, the dock layout on a fresh `imgui.ini`, and the lock (Tab and the arrows do nothing; 1–9, Ctrl+B and F11 work; in perform mode, Esc on "Unlock the Show?" keeps the projectors up and 1–9 still take scenes while it is open; locking with the Blend Test Pattern on turns it off; a file dialog opened before locking and answered after it changes nothing).
- **Size:** `Application.cpp` is under 1,200 lines by E5. If E5 slips, moving scene takes, external control and component setup into `app/ShowControl.cpp` gets there without the UI rework.

## Consequences
- ✅ Editor logic is testable without ImGui or a window. The layering is enforced by a test, not by convention.
- ✅ One place to add a panel and one place to add a shortcut. The menu cannot disagree with the keys.
- ✅ A live show can be locked against accidental edits without losing show control.
- ❌ Header moves touch about 70 include lines. The history of moved files needs `git log --follow`.
- ❌ Exact modifier matching changes two accidental behaviours: Ctrl+Shift+S and Ctrl+Alt+M no longer fire Save and Mesh.
- ❌ Controls that edit the model directly, outside commands, are covered by the lock only through `BeginDisabled`. New panels must keep `editsShow` true unless they are show controls.

## Alternatives considered
1. **An INTERFACE library of headers in `src/app`.** Rejected: no `.cpp` homes, and no link-level proof that the cycle is gone.
2. **Routing canvas keys through the registry.** Rejected: the canvas keys are modal and repeat, and Shift/Alt are step sizes. Putting them behind a chord table risks the success metric for no gain.
3. **A persistent perform lock saved in the show.** Rejected: a show opened on another machine would open locked. The lock belongs to the session, like blackout.

## Out of scope
The renderer↔source include cycle (`GpuCompressedFormat` in `source/FrameData.hpp`) is a separate hygiene item.
