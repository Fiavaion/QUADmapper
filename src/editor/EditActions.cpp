#include "EditActions.hpp"

#include "EditorContext.hpp"
#include "EditorState.hpp"
#include "UndoRouting.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "scene/QuadGeometry.hpp"

#include <imgui.h>  // ImGuiKey values for the chords; no ImGui calls

#include <cassert>
#include <cstdio>

namespace quadmapper {

namespace {

// Alt+- stops at 2 (the key block's floor), above QuadGeometry's MIN_MESH_RESOLUTION.
constexpr int MIN_KEY_MESH_RESOLUTION = 2;

// Undo or redo, posting why a held-back step was not taken.
bool historyStep(EditorState& es, bool undo) {
    std::string heldBack;
    if (undo ? undoStep(es, &heldBack) : redoStep(es, &heldBack)) return true;
    if (!heldBack.empty()) {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Leave %s to %s: %s",
                 undoScopeName(es), undo ? "undo" : "redo", heldBack.c_str());
        es.statusTimer = 3.0f;
    }
    return false;
}

QuadGeometry* primaryGeometry(EditorState& es) {
    if (!quadEditing(es)) return nullptr;
    Quad* quad = es.quadManager().getPrimarySelectedQuad();
    return quad ? &quad->getGeometry() : nullptr;
}

Action editAction(const char* id, const char* label, std::vector<Chord> chords,
                  std::function<bool(const EditorContext&)> enabled,
                  std::function<void(EditorContext&)> run) {
    Action action;
    action.id = id;
    action.label = label;
    action.chords = std::move(chords);
    action.cls = ActionClass::Edit;
    action.guard = KeyGuard::Editing;
    action.enabled = std::move(enabled);
    action.run = std::move(run);
    return action;
}

}  // namespace

std::string sceneTakeId(int n) { return "scene.take." + std::to_string(n); }

bool quadEditing(const EditorState& es) {
    return es.calibratingOutputId.isNull() && es.sliceWarpOutputId.isNull();
}

bool undoAction(EditorState& es) { return historyStep(es, true); }
bool redoAction(EditorState& es) { return historyStep(es, false); }

bool canSelectAll(EditorState& es) { return quadEditing(es); }

void selectAllAction(EditorState& es) {
    if (canSelectAll(es)) es.quadManager().selectAll();
}

void deselectAllAction(EditorState& es) { es.quadManager().deselectAll(); }

bool canDeleteSelection(EditorState& es, const OutputManager* outputs) {
    if (!es.calibratingOutputId.isNull()) {
        return outputs && outputs->getOutput(es.calibratingOutputId) &&
               es.calibration.selected >= 0;
    }
    if (!quadEditing(es)) return false;
    if (es.quadManager().hasSelection()) return true;
    return es.currentSelectionType == EditorState::SelectionType::Object &&
           !es.selectedObjectId.isNull() && es.activeScene().getObject(es.selectedObjectId) != nullptr;
}

bool deleteSelectionAction(EditorState& es, OutputManager* outputs) {
    if (!es.calibratingOutputId.isNull()) {
        // Calibrating: Delete removes the selected calibration point, never quads.
        OutputWindow* calibrated = outputs ? outputs->getOutput(es.calibratingOutputId) : nullptr;
        if (!calibrated || es.calibration.selected < 0) return false;
        es.calibration.noteEdit("Delete calibration point " +
                                std::to_string(es.calibration.selected + 1));
        es.calibration.removePoint(calibrated->getCalibrationPoints(), es.calibration.selected);
        return true;
    }
    // Slice warp editing: the quads are hidden, Delete leaves them alone.
    if (!quadEditing(es)) return false;
    // Selecting an object deselects the quads, so a quad selection is newer.
    if (es.quadManager().hasSelection()) {
        es.deleteSelectedQuads();
        return true;
    }
    return es.deleteSelectedObject();
}

bool canMoveZ(EditorState& es) { return quadEditing(es) && es.quadManager().hasSelection(); }

void moveZAction(EditorState& es, ZMove move) {
    if (!canMoveZ(es)) return;
    QuadManager& quads = es.quadManager();
    switch (move) {
        case ZMove::Forward:  quads.moveSelectedUp(); break;        // render later = on top
        case ZMove::Backward: quads.moveSelectedDown(); break;      // render earlier = behind
        case ZMove::ToFront:  quads.moveSelectedToTop(); break;
        case ZMove::ToBack:   quads.moveSelectedToBottom(); break;
    }
}

bool canToggleMeshEdit(EditorState& es) { return primaryGeometry(es) != nullptr; }

void toggleMeshEditAction(EditorState& es) {
    QuadGeometry* geo = primaryGeometry(es);
    if (!geo) return;
    const bool isEdit = geo->getMeshMode() == MeshMode::MESH_EDIT;
    geo->setMeshMode(isEdit ? MeshMode::CORNERS_ONLY : MeshMode::MESH_EDIT);
}

bool canStepMeshResolution(EditorState& es, int delta) {
    const QuadGeometry* geo = primaryGeometry(es);
    if (!geo || delta == 0) return false;
    const int res = geo->getMeshResolution();
    return delta > 0 ? res < MAX_MESH_RESOLUTION : res > MIN_KEY_MESH_RESOLUTION;
}

void stepMeshResolutionAction(EditorState& es, int delta) {
    if (!canStepMeshResolution(es, delta)) return;
    QuadGeometry* geo = primaryGeometry(es);
    geo->setMeshResolution(geo->getMeshResolution() + (delta > 0 ? 1 : -1));
}

bool canResetMesh(EditorState& es) {
    const QuadGeometry* geo = primaryGeometry(es);
    return geo && geo->getMeshMode() == MeshMode::MESH_EDIT && geo->hasMeshModifications();
}

void resetMeshAction(EditorState& es) {
    // Residuals and grid handles back to the corner-pin surface.
    if (canResetMesh(es)) primaryGeometry(es)->resetMesh();
}

void registerEditActions(ActionRegistry& registry, std::function<void()> historyChanged) {
    // Every default chord is distinct, so an add can only fail on a programming error.
    const auto put = [&registry](Action action) {
        const AddResult added = registry.add(std::move(action));
        assert(added == AddResult::Added);
        (void)added;
    };

    // Undo / redo stay enabled while a step is held back, so the key (and the menu)
    // can say why nothing happened.
    put(editAction(
        action_id::UNDO, "Undo", {Chord{ImGuiKey_Z, MOD_CTRL}},
        [](const EditorContext& ctx) { return ctx.es.commandHistory.peekUndo() != nullptr; },
        [historyChanged](EditorContext& ctx) {
            if (undoAction(ctx.es) && historyChanged) historyChanged();
        }));
    // Redo on Ctrl+Y and on Ctrl+Shift+Z (owner's decision, 2026-09-30).
    put(editAction(
        action_id::REDO, "Redo", {Chord{ImGuiKey_Y, MOD_CTRL}, Chord{ImGuiKey_Z, MOD_CTRL | MOD_SHIFT}},
        [](const EditorContext& ctx) { return ctx.es.commandHistory.peekRedo() != nullptr; },
        [historyChanged](EditorContext& ctx) {
            if (redoAction(ctx.es) && historyChanged) historyChanged();
        }));

    put(editAction(
        action_id::SELECT_ALL, "Select All", {Chord{ImGuiKey_A, MOD_CTRL}},
        [](const EditorContext& ctx) { return canSelectAll(ctx.es); },
        [](EditorContext& ctx) { selectAllAction(ctx.es); }));
    // Menu only: Esc is the perform / calibration / learn chain (perform.escape).
    put(editAction(
        action_id::DESELECT_ALL, "Deselect All", {}, {},
        [](EditorContext& ctx) { deselectAllAction(ctx.es); }));
    put(editAction(
        action_id::DELETE_SELECTION, "Delete Selected", {Chord{ImGuiKey_Delete, MOD_NONE}},
        [](const EditorContext& ctx) { return canDeleteSelection(ctx.es, ctx.outputManager); },
        [](EditorContext& ctx) { deleteSelectionAction(ctx.es, ctx.outputManager); }));

    const auto zOrder = [&](const char* id, const char* label, Chord chord, ZMove move) {
        put(editAction(
            id, label, {chord}, [](const EditorContext& ctx) { return canMoveZ(ctx.es); },
            [move](EditorContext& ctx) { moveZAction(ctx.es, move); }));
    };
    zOrder(action_id::BRING_FORWARD, "Bring Forward", Chord{ImGuiKey_RightBracket, MOD_CTRL},
           ZMove::Forward);
    zOrder(action_id::SEND_BACKWARD, "Send Backward", Chord{ImGuiKey_LeftBracket, MOD_CTRL},
           ZMove::Backward);
    zOrder(action_id::BRING_TO_FRONT, "Bring to Front",
           Chord{ImGuiKey_RightBracket, MOD_CTRL | MOD_SHIFT}, ZMove::ToFront);
    zOrder(action_id::SEND_TO_BACK, "Send to Back",
           Chord{ImGuiKey_LeftBracket, MOD_CTRL | MOD_SHIFT}, ZMove::ToBack);

    put(editAction(
        action_id::TOGGLE_MESH_EDIT, "Toggle Mesh Edit", {Chord{ImGuiKey_M, MOD_CTRL}},
        [](const EditorContext& ctx) { return canToggleMeshEdit(ctx.es); },
        [](EditorContext& ctx) { toggleMeshEditAction(ctx.es); }));
    put(editAction(
        action_id::MESH_FINER, "Increase Mesh Resolution", {Chord{ImGuiKey_Equal, MOD_ALT}},
        [](const EditorContext& ctx) { return canStepMeshResolution(ctx.es, +1); },
        [](EditorContext& ctx) { stepMeshResolutionAction(ctx.es, +1); }));
    put(editAction(
        action_id::MESH_COARSER, "Decrease Mesh Resolution", {Chord{ImGuiKey_Minus, MOD_ALT}},
        [](const EditorContext& ctx) { return canStepMeshResolution(ctx.es, -1); },
        [](EditorContext& ctx) { stepMeshResolutionAction(ctx.es, -1); }));
    put(editAction(
        action_id::RESET_MESH, "Reset Mesh", {Chord{ImGuiKey_R, MOD_ALT}},
        [](const EditorContext& ctx) { return canResetMesh(ctx.es); },
        [](EditorContext& ctx) { resetMeshAction(ctx.es); }));
}

void registerFileActions(ActionRegistry& registry) {
    using FileAction = EditorState::FileAction;
    const auto file = [&registry](const char* id, const char* label, Chord chord, FileAction request) {
        Action action;
        action.id = id;
        action.label = label;
        action.chords = {chord};
        action.cls = ActionClass::File;
        action.run = [request](EditorContext& ctx) { ctx.es.pendingFileAction = request; };
        const AddResult added = registry.add(std::move(action));
        assert(added == AddResult::Added);
        (void)added;
    };
    file(action_id::NEW_PROJECT, "New Project", Chord{ImGuiKey_N, MOD_CTRL}, FileAction::New);
    file(action_id::OPEN_PROJECT, "Open Project...", Chord{ImGuiKey_O, MOD_CTRL}, FileAction::Open);
    file(action_id::SAVE_AS, "Save Project As...", Chord{ImGuiKey_S, MOD_CTRL | MOD_SHIFT},
         FileAction::SaveAs);
}

std::optional<Action> makeAppAction(std::string_view id, std::function<void(EditorContext&)> run) {
    Action action;
    action.id = std::string(id);
    action.run = std::move(run);
    if (id == action_id::SAVE) {
        action.label = "Save Project";
        action.chords = {Chord{ImGuiKey_S, MOD_CTRL}};
        action.cls = ActionClass::File;
    } else if (id == action_id::PREFERENCES) {
        action.label = "Preferences...";
        action.chords = {Chord{ImGuiKey_Comma, MOD_CTRL}};
        action.cls = ActionClass::View;
    } else if (id == action_id::BLACKOUT) {
        // The panic key: works with a popup open, only a text field keeps it.
        action.label = "Blackout";
        action.chords = {Chord{ImGuiKey_B, MOD_CTRL}};
        action.cls = ActionClass::Show;
        action.guard = KeyGuard::Show;
        action.fixed = true;
    } else if (id == action_id::PERFORM_TOGGLE) {
        action.label = "Perform Mode";
        action.chords = {Chord{ImGuiKey_F11, MOD_NONE}};
        action.cls = ActionClass::Show;
        action.guard = KeyGuard::Show;
        action.fixed = true;
    } else if (id == action_id::PERFORM_ESCAPE) {
        // Answers a session modal only; else cancels a MIDI learn, then leaves slice warp
        // editing, a calibration pick, calibration, or else perform mode (performEscape).
        action.label = "Escape";
        action.chords = {Chord{ImGuiKey_Escape, MOD_NONE}};
        action.cls = ActionClass::Show;
        action.guard = KeyGuard::Show;
        action.fixed = true;
    } else if (id == action_id::PERFORM_LOCK) {
        // The perform lock (ADR-051 §4): Show class, so the lock never refuses its own
        // way out.
        action.label = "Perform Lock";
        action.chords = {Chord{ImGuiKey_L, MOD_CTRL | MOD_SHIFT}};
        action.cls = ActionClass::Show;
    } else {
        // Scene takes 1-9 have no chord: the physical key is captured outside
        // (layout-independent) and queued, and the queue drains under the Editing guard.
        int n = 0;
        for (int i = 1; i <= 9 && n == 0; ++i) {
            if (id == sceneTakeId(i)) n = i;
        }
        if (n == 0) return std::nullopt;
        action.label = "Take Scene " + std::to_string(n);
        action.cls = ActionClass::Show;
    }
    return action;
}

}  // namespace quadmapper
