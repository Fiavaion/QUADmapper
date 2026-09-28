#pragma once

// The edit.* actions (ADR-051 §3): the editor-wide editing keys, with the behaviour and
// guards they had in Application::render()'s shortcut block. Each is a function over
// EditorState, tested on its own (EditActionsTest), and registerEditActions() puts them
// in the registry on their existing chords. The ids and chords of the actions the app
// registers (AppActions: they need app services) are defined here too, so every chord
// lives in one place.

#include "ActionRegistry.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace quadmapper {

struct EditorState;
class OutputManager;

namespace action_id {
// Edit actions (registerEditActions).
inline constexpr const char* UNDO = "edit.undo";
inline constexpr const char* REDO = "edit.redo";
inline constexpr const char* SELECT_ALL = "edit.selectAll";
inline constexpr const char* DESELECT_ALL = "edit.deselectAll";
inline constexpr const char* DELETE_SELECTION = "edit.delete";
inline constexpr const char* BRING_FORWARD = "edit.bringForward";
inline constexpr const char* SEND_BACKWARD = "edit.sendBackward";
inline constexpr const char* BRING_TO_FRONT = "edit.bringToFront";
inline constexpr const char* SEND_TO_BACK = "edit.sendToBack";
inline constexpr const char* TOGGLE_MESH_EDIT = "edit.toggleMeshEdit";
inline constexpr const char* MESH_FINER = "edit.meshFiner";
inline constexpr const char* MESH_COARSER = "edit.meshCoarser";
inline constexpr const char* RESET_MESH = "edit.resetMesh";
// App actions (makeAppAction).
inline constexpr const char* SAVE = "file.save";
inline constexpr const char* PREFERENCES = "prefs.open";
inline constexpr const char* BLACKOUT = "output.blackout";
inline constexpr const char* PERFORM_TOGGLE = "perform.toggle";
inline constexpr const char* PERFORM_ESCAPE = "perform.escape";
}  // namespace action_id

// "scene.take.N" for scene take N (1-9).
std::string sceneTakeId(int n);

// ---- The edit actions over EditorState --------------------------------------------

// The quad shortcuts act only while quads are being edited: not while a projector is
// calibrated or a slice warp is edited (the quads are hidden then). The can*
// predicates take a mutable EditorState because activeScene() may create the first
// scene.
bool quadEditing(const EditorState& es);

// Undo / redo the next step, scoped by UndoRouting (TD-17). True when a step was taken.
// A step held back by calibration or slice warp editing posts "Leave <mode> to
// undo: <step>" on the status line.
bool undoAction(EditorState& es);
bool redoAction(EditorState& es);

bool canSelectAll(EditorState& es);
void selectAllAction(EditorState& es);
void deselectAllAction(EditorState& es);

// Delete: while calibrating, the selected calibration point of `outputs`' calibrated
// output (never quads); while editing a slice warp, nothing; else the selected quads,
// or, with none, the selected 3D object. True when something was deleted.
bool canDeleteSelection(EditorState& es, const OutputManager* outputs);
bool deleteSelectionAction(EditorState& es, OutputManager* outputs);

enum class ZMove { Forward, Backward, ToFront, ToBack };
bool canMoveZ(EditorState& es);
void moveZAction(EditorState& es, ZMove move);

// Mesh keys on the primary selected quad: Ctrl+M toggles mesh edit, Alt+= / Alt+-
// step the resolution within 2..64, Alt+R resets a modified mesh in mesh edit.
bool canToggleMeshEdit(EditorState& es);
void toggleMeshEditAction(EditorState& es);
bool canStepMeshResolution(EditorState& es, int delta);
void stepMeshResolutionAction(EditorState& es, int delta);
bool canResetMesh(EditorState& es);
void resetMeshAction(EditorState& es);

// Register every edit action on its chord. `historyChanged` runs after an undo or redo
// took a step (the app refreshes its unsaved-changes label).
void registerEditActions(ActionRegistry& registry, std::function<void()> historyChanged = {});

// The app action with this id (action_id::SAVE ... PERFORM_ESCAPE, sceneTakeId(n)),
// with its label, chord, class and guard, running `run`. nullopt for any other id.
std::optional<Action> makeAppAction(std::string_view id, std::function<void(EditorContext&)> run);

}  // namespace quadmapper
