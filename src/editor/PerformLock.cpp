#include "PerformLock.hpp"

#include "ActionRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorState.hpp"
#include "StatusLine.hpp"
#include "control/ExternalControl.hpp"
#include "core/Logger.hpp"

namespace quadmapper {

void lockPerform(EditorState& es) {
    if (es.performLock.on_) return;
    // Leave the editing modes while the history still records: a warp corner drag in
    // progress becomes its undo step. The canvas then shows the quads, uneditable.
    es.endSliceWarp();
    es.endCalibration();
    es.gridInsert = EditorState::GridInsert::None;
    es.gridInsertMouseLatch = false;
    es.gridInsertDragIndex = -1;
    es.commandHistory.setReadOnly(true);
    es.performLock.on_ = true;
    es.performLock.confirmingUnlock_ = false;
    Logger::info("Perform lock on");
    postStatus(es, "Perform lock on: show edits are blocked (Output > Perform Lock to unlock)");
}

void requestPerformUnlock(EditorState& es) {
    if (es.performLock.on_) es.performLock.confirmingUnlock_ = true;
}

bool confirmPerformUnlock(EditorState& es) {
    if (!es.performLock.on_ || !es.performLock.confirmingUnlock_) return false;
    es.performLock.on_ = false;
    es.performLock.confirmingUnlock_ = false;
    es.commandHistory.setReadOnly(false);
    Logger::info("Perform lock off");
    postStatus(es, "Perform lock off: the show can be edited");
    return true;
}

void cancelPerformUnlock(EditorState& es) { es.performLock.confirmingUnlock_ = false; }

bool performLockRefuses(const EditorState& es, const Action& action) {
    return es.performLock.isOn() && action.cls == ActionClass::Edit;
}

bool refusedByPerformLock(EditorState& es, const std::string& what) {
    if (!es.performLock.isOn()) return false;
    postStatus(es, what + " is blocked: the show is locked (Output > Perform Lock)");
    return true;
}

void togglePerformLock(EditorContext& ctx) {
    if (ctx.es.performLock.isOn()) {
        requestPerformUnlock(ctx.es);
        return;
    }
    // A MIDI learn waiting for its control would edit the show's mappings.
    if (ctx.control) ctx.control->dispatcher().cancelLearn();
    lockPerform(ctx.es);
}

void installPerformLockPolicy(ActionRegistry& registry, EditorState& es) {
    registry.setLockPolicy([&es](const Action& action, Source) {
        if (!performLockRefuses(es, action)) return false;
        refusedByPerformLock(es, action.label);
        return true;
    });
}

}  // namespace quadmapper
