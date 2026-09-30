#pragma once

// The perform lock (ADR-051 §4): during a live show it blocks show edits (the Edit
// actions, the canvas, media drops, show-editing panels, New / Open / Load / Import /
// Restore, calibration and slice warp editing, AI assist, MIDI learn, Preferences) and
// keeps show control (scene takes, fades, blackout, F11 / Esc, OSC / MIDI, the Video
// Player transport, Save and autosave, canvas zoom / pan, Exit).
//
// Runtime state, like blackout: held in EditorState, never saved (no .qmap change).
// Locking is immediate; unlocking needs a confirmation (the UI's modal answers
// confirmPerformUnlock or cancelPerformUnlock).

#include <string>

namespace quadmapper {

struct EditorState;
struct EditorContext;
struct Action;
class ActionRegistry;

class PerformLock {
public:
    bool isOn() const { return on_; }
    // An unlock was asked for and waits for its confirmation.
    bool isConfirmingUnlock() const { return confirmingUnlock_; }

private:
    friend void lockPerform(EditorState& es);
    friend void requestPerformUnlock(EditorState& es);
    friend bool confirmPerformUnlock(EditorState& es);
    friend void cancelPerformUnlock(EditorState& es);

    bool on_ = false;
    bool confirmingUnlock_ = false;
};

// Lock now. An active slice warp edit or calibration ends first, and every canvas drag
// in progress (a corner, mesh vertex, grid handle, Bezier handle, warp corner or a
// division being inserted) is kept where it is as its undo step. Then the grid insert
// tool is disarmed and the command history turns read-only (the safety net).
void lockPerform(EditorState& es);
// Ask to unlock: nothing changes until confirmPerformUnlock.
void requestPerformUnlock(EditorState& es);
// Unlock, when an unlock was asked for. False (still locked) otherwise.
bool confirmPerformUnlock(EditorState& es);
void cancelPerformUnlock(EditorState& es);

// Whether the lock refuses this registered action now: while it is on, the Edit class
// and File > New and Open (Save and Save As stay allowed).
bool performLockRefuses(const EditorState& es, const Action& action);

// While locked, posts "<what> is blocked: the show is locked (Output > Perform Lock)"
// on the status line and returns true. False, and nothing posted, when unlocked.
bool refusedByPerformLock(EditorState& es, const std::string& what);

// The perform.lock action: locks (cancelling a pending MIDI learn and turning the blend
// test pattern off, so the projectors show the show), or, while locked, asks for the
// unlock confirmation.
void togglePerformLock(EditorContext& ctx);

// The perform.escape action (Esc, which acts with a popup open). While a session modal
// is up (Unlock the Show?, Unsaved Changes, Recover Unsaved Work) Esc answers only the
// modal: the unlock prompt stays locked, and nothing else is left, perform mode least
// of all. Otherwise it cancels a pending MIDI learn, then leaves slice warp editing, a
// calibration pick, calibration, or else perform mode.
void performEscape(EditorContext& ctx);

// The perform.toggle action (F11): perform mode on or off. With no outputs F11 creates
// "Projector 1" first; under the lock it refuses that (outputs are show content).
void performToggle(EditorContext& ctx);

// Installs the registry's lock policy: refuses what performLockRefuses says, posting
// why on the status line. `es` must outlive the registry's use of it.
void installPerformLockPolicy(ActionRegistry& registry, EditorState& es);

}  // namespace quadmapper
