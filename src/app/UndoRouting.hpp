#pragma once

// Which step Ctrl+Z / Ctrl+Y and Edit > Undo / Redo take (TD-17).
//
// There is one history, in the order things were done: quad, 3D-object and calibration
// steps together. While a projector is being calibrated, only that output's calibration
// steps are undone or redone. When the next step is anything else (a quad edit made
// before calibration started, or another output's calibration) nothing happens and the
// caller says why, so a Ctrl+Z meant for a misplaced point never moves a quad hidden
// behind the calibration view. Outside calibration every step is taken in order; a
// calibration step still applies to its output by id and is a no-op once that output
// is gone.

#include "EditorState.hpp"
#include "scene/Commands.hpp"

#include <string>

namespace quadmapper {

inline bool isUndoStepAllowed(const EditorState& es, const Command* step) {
    if (!step) return false;
    if (es.calibratingOutputId.isNull()) return true;
    const auto* calibration = dynamic_cast<const CalibrationEditCommand*>(step);
    return calibration && calibration->getOutputId() == es.calibratingOutputId;
}

inline bool canUndoStep(const EditorState& es) {
    return isUndoStepAllowed(es, es.commandHistory.peekUndo());
}

inline bool canRedoStep(const EditorState& es) {
    return isUndoStepAllowed(es, es.commandHistory.peekRedo());
}

// Undo the next step if it may be taken now. True when something was undone. When the
// step is held back, `heldBack` receives its description.
inline bool undoStep(EditorState& es, std::string* heldBack = nullptr) {
    const Command* step = es.commandHistory.peekUndo();
    if (!isUndoStepAllowed(es, step)) {
        if (heldBack && step) *heldBack = step->getDescription();
        return false;
    }
    return es.commandHistory.undo();
}

inline bool redoStep(EditorState& es, std::string* heldBack = nullptr) {
    const Command* step = es.commandHistory.peekRedo();
    if (!isUndoStepAllowed(es, step)) {
        if (heldBack && step) *heldBack = step->getDescription();
        return false;
    }
    return es.commandHistory.redo();
}

}  // namespace quadmapper
