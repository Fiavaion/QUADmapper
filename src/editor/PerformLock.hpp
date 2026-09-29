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

// Lock now. An active slice warp edit or calibration ends first (a warp corner drag in
// progress is kept as its undo step), the grid insert tool and canvas drags are
// dropped, and the command history turns read-only (the safety net).
void lockPerform(EditorState& es);
// Ask to unlock: nothing changes until confirmPerformUnlock.
void requestPerformUnlock(EditorState& es);
// Unlock, when an unlock was asked for. False (still locked) otherwise.
bool confirmPerformUnlock(EditorState& es);
void cancelPerformUnlock(EditorState& es);

// Whether the lock refuses this registered action now: the Edit class while it is on.
bool performLockRefuses(const EditorState& es, const Action& action);

// While locked, posts "<what> is blocked: the show is locked (Output > Perform Lock)"
// on the status line and returns true. False, and nothing posted, when unlocked.
bool refusedByPerformLock(EditorState& es, const std::string& what);

// The perform.lock action: locks (cancelling a pending MIDI learn), or, while locked,
// asks for the unlock confirmation.
void togglePerformLock(EditorContext& ctx);

// Installs the registry's lock policy: refuses what performLockRefuses says, posting
// why on the status line. `es` must outlive the registry's use of it.
void installPerformLockPolicy(ActionRegistry& registry, EditorState& es);

}  // namespace quadmapper
