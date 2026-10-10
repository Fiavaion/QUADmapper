#include "PerformLock.hpp"

#include "ActionRegistry.hpp"
#include "EditActions.hpp"
#include "EditorContext.hpp"
#include "EditorState.hpp"
#include "GridUndo.hpp"
#include "StatusLine.hpp"
#include "control/ExternalControl.hpp"
#include "core/Logger.hpp"
#include "output/OutputManager.hpp"
#include "scene/Commands.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"

#include <array>
#include <memory>

namespace quadmapper {

namespace {

// The canvas's drags on the selected quad (ViewportEditor) end where the mouse left
// them, each as the undo step its release would have recorded; then the grid insert
// tool is disarmed. Called while the history still records.
void commitCanvasDrags(EditorState& es) {
    if (Quad* quad = es.quadManager().getPrimarySelectedQuad()) {
        QuadGeometry& geometry = quad->getGeometry();
        CommandHistory& history = es.commandHistory;
        if (es.gridInsertMouseLatch && es.gridInsertDragIndex >= 0) {
            pushGridChange(history, es.quadRef(*quad), "Insert Division", es.gridInsertBefore,
                           captureGrid(geometry));
        }
        if (geometry.getMeshMode() == MeshMode::CORNERS_ONLY) {
            const int corner = es.cornerDraggedVertex;
            if (corner >= 0 && corner < 4) {
                const Vec2 now = geometry.getCorners()[static_cast<size_t>(corner)];
                if (now != es.cornerDragStartPos) {
                    history.execute(std::make_unique<MoveCornerCommand>(
                        es.quadRef(*quad), corner, es.cornerDragStartPos, now));
                }
                es.cornerSelectedVertex = corner;
            }
            if (es.bezierDragged >= 0 && geometry.getBezierOffsets() != es.bezierDragBefore) {
                history.execute(std::make_unique<QuadPropertyCommand<std::array<Vec2, 8>>>(
                    "Bezier Edge", es.quadRef(*quad), es.bezierDragBefore, geometry.getBezierOffsets(),
                    [](Quad& q, const std::array<Vec2, 8>& o) { q.getGeometry().setBezierOffsets(o); }));
            }
        } else if (es.meshDraggedHandle >= 0 && es.meshSelectedVertex >= 0) {
            const VertexTangents after = geometry.getVertexTangent(es.meshSelectedVertex);
            if (after != es.meshHandleDragBefore) {
                history.execute(std::make_unique<MoveGridHandleCommand>(
                    es.quadRef(*quad), es.meshSelectedVertex, es.meshDraggedHandle,
                    es.meshHandleDragBefore, after));
            }
            es.meshSelectedHandle = es.meshDraggedHandle;
        } else if (es.meshDraggedVertex >= 0) {
            const Vec2 offset = geometry.getMeshVertexByIndex(es.meshDraggedVertex);
            if (offset != es.meshDragStartOffset) {
                history.execute(std::make_unique<MoveMeshVertexCommand>(
                    es.quadRef(*quad), es.meshDraggedVertex, es.meshDragStartOffset, offset));
            }
            es.meshSelectedVertex = es.meshDraggedVertex;
        }
    }
    es.cornerDraggedVertex = -1;
    es.meshDraggedVertex = -1;
    es.meshDraggedHandle = -1;
    es.bezierDragged = -1;
    es.gridInsert = EditorState::GridInsert::None;
    es.gridInsertMouseLatch = false;
    es.gridInsertDragIndex = -1;
}

}  // namespace

void lockPerform(EditorState& es) {
    if (es.performLock.on_) return;
    // Leave the editing modes and end the drags while the history still records: a
    // drag in progress becomes its undo step. The canvas then shows the quads,
    // uneditable.
    es.endSliceWarp();
    es.endCalibration();
    commitCanvasDrags(es);
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
    if (!es.performLock.isOn()) return false;
    // New and Open would replace the show on air; Save and Save As keep it.
    return action.cls == ActionClass::Edit || action.id == action_id::NEW_PROJECT ||
           action.id == action_id::OPEN_PROJECT;
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
    // The blend test pattern replaces the show on every projector, and its switch is in
    // the Inspector, which the lock disables: locking puts the show back on them.
    if (ctx.outputManager && ctx.outputManager->isBlendTestPattern()) {
        ctx.outputManager->setBlendTestPattern(false);
        postStatus(ctx.es, "Perform lock on: the blend test pattern is off and show edits are "
                           "blocked (Output > Perform Lock to unlock)");
    }
}

void performEscape(EditorContext& ctx) {
    EditorState& es = ctx.es;
    // The session modals are drawn after the shortcuts, so this same Esc reaches them
    // too: it is theirs alone.
    if (es.performLock.isConfirmingUnlock()) {
        cancelPerformUnlock(es);  // "Stay Locked"
        return;
    }
    if (es.unsavedPromptFor != EditorState::FileAction::None || !es.recoveryFilePath.empty()) return;

    if (ctx.control) ctx.control->dispatcher().cancelLearn();
    // A running scan owns Esc in every phase (calibration and slice warp editing cannot
    // start during one): it is cancelled, which also gives back the perform mode it took.
    if (es.aiScan.running) {
        es.aiScan.cancelRequested = true;
        return;
    }
    // Slice warp editing takes Esc: it leaves warp editing. While calibrating, Esc
    // belongs to calibration: it cancels a pick in progress, else leaves calibration,
    // and never also leaves perform mode (Esc on an output window still does).
    if (!es.sliceWarpOutputId.isNull()) {
        es.endSliceWarp();
    } else if (es.calibratingOutputId.isNull()) {
        if (ctx.outputManager) ctx.outputManager->handleEscapeKey();
    } else if (es.calibration.pick != CalibrationSession::Pick::None) {
        es.calibration.cancelPick();
    } else {
        es.endCalibration();
    }
}

void performToggle(EditorContext& ctx) {
    OutputManager* outputs = ctx.outputManager;
    if (!outputs) return;
    if (!outputs->isInPerformMode() && outputs->getOutputCount() == 0 &&
        refusedByPerformLock(ctx.es, "Creating a projector output")) {
        return;
    }
    outputs->handleF11Key();
}

void installPerformLockPolicy(ActionRegistry& registry, EditorState& es) {
    registry.setLockPolicy([&es](const Action& action, Source) {
        if (!performLockRefuses(es, action)) return false;
        refusedByPerformLock(es, action.label);
        return true;
    });
}

}  // namespace quadmapper
