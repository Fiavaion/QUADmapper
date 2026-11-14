// The edit.* actions (editor/EditActions): each does what Application::render()'s
// shortcut block did for its key, with the same guards, invoked through the registry
// as the keys and the menu will be. OutputManager runs headless here (no windows).

#include "editor/ActionRegistry.hpp"
#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"

#include <imgui.h>

#include <gtest/gtest.h>

#include <string>

using namespace quadmapper;

namespace {

struct Rig {
    EditorState es;
    OutputManager om;
    EditorContext ctx{es, nullptr, nullptr, nullptr, &om, nullptr, nullptr};
    ActionRegistry registry;
    int historyChanges = 0;
    Quad* a = nullptr;
    Quad* b = nullptr;
    Quad* c = nullptr;

    Rig() {
        es.outputResolver.setOutputManager(&om);
        registerEditActions(registry, [this] { ++historyChanges; });
        a = es.quadManager().createQuad("A");
        b = es.quadManager().createQuad("B");
        c = es.quadManager().createQuad("C");
    }

    Outcome run(const char* id) { return registry.invoke(id, ctx, Source::Key); }
    size_t quadCount() { return es.quadManager().getQuadCount(); }
    int indexOf(const Quad* quad) {
        const auto& quads = es.quadManager().getQuads();
        for (size_t i = 0; i < quads.size(); ++i) {
            if (quads[i].get() == quad) return static_cast<int>(i);
        }
        return -1;
    }
    OutputWindow* calibrateWithPoints(int points) {
        OutputWindow* output = om.createOutput("Projector");
        output->getCalibrationPoints().resize(static_cast<size_t>(points));
        es.beginCalibration(*output);
        return output;
    }
};

}  // namespace

TEST(EditActionsTest, UndoAndRedoTakeTheStepAndReportIt) {
    Rig rig;
    rig.es.quadManager().selectQuad(rig.a->getId());
    ASSERT_EQ(rig.run(action_id::DELETE_SELECTION), Outcome::Ran);
    EXPECT_EQ(rig.quadCount(), 2u);

    EXPECT_EQ(rig.run(action_id::UNDO), Outcome::Ran);
    EXPECT_EQ(rig.quadCount(), 3u);
    EXPECT_EQ(rig.historyChanges, 1);
    EXPECT_EQ(rig.run(action_id::REDO), Outcome::Ran);
    EXPECT_EQ(rig.quadCount(), 2u);
    EXPECT_EQ(rig.historyChanges, 2);
    EXPECT_EQ(rig.run(action_id::REDO), Outcome::Disabled) << "nothing to redo";
    EXPECT_EQ(rig.historyChanges, 2);
}

// TD-17: while calibrating, a quad step is held back and the status line says why.
TEST(EditActionsTest, UndoIsScopedToCalibration) {
    Rig rig;
    rig.es.quadManager().selectQuad(rig.a->getId());
    rig.run(action_id::DELETE_SELECTION);
    rig.calibrateWithPoints(0);

    EXPECT_EQ(rig.run(action_id::UNDO), Outcome::Ran) << "enabled so the key can say why";
    EXPECT_EQ(rig.quadCount(), 2u) << "the hidden quad is not restored";
    EXPECT_EQ(rig.historyChanges, 0);
    EXPECT_EQ(std::string(rig.es.statusMessage), "Leave calibration to undo: Delete quad");
    EXPECT_FLOAT_EQ(rig.es.statusTimer, 3.0f);

    rig.es.endCalibration();
    EXPECT_EQ(rig.run(action_id::UNDO), Outcome::Ran);
    EXPECT_EQ(rig.quadCount(), 3u);
}

TEST(EditActionsTest, UndoIsScopedToSliceWarpEditing) {
    Rig rig;
    rig.es.quadManager().selectQuad(rig.a->getId());
    rig.run(action_id::DELETE_SELECTION);
    rig.run(action_id::UNDO);
    rig.es.sliceWarpOutputId = UUID::generate();

    EXPECT_FALSE(redoAction(rig.es));
    EXPECT_EQ(rig.quadCount(), 3u);
    EXPECT_EQ(std::string(rig.es.statusMessage), "Leave slice warp editing to redo: Delete quad");
}

TEST(EditActionsTest, SelectAllKeepsOffWhileCalibratingOrWarping) {
    Rig rig;
    EXPECT_EQ(rig.run(action_id::SELECT_ALL), Outcome::Ran);
    EXPECT_EQ(rig.es.quadManager().getSelectionCount(), 3u);

    EXPECT_EQ(rig.run(action_id::DESELECT_ALL), Outcome::Ran);
    EXPECT_FALSE(rig.es.quadManager().hasSelection());

    rig.calibrateWithPoints(0);
    EXPECT_EQ(rig.run(action_id::SELECT_ALL), Outcome::Disabled);
    EXPECT_FALSE(rig.es.quadManager().hasSelection());
    rig.es.endCalibration();

    rig.es.sliceWarpOutputId = UUID::generate();
    EXPECT_EQ(rig.run(action_id::SELECT_ALL), Outcome::Disabled);
    EXPECT_FALSE(rig.es.quadManager().hasSelection());
}

TEST(EditActionsTest, DeleteRemovesSelectedQuadsAsOneStep) {
    Rig rig;
    rig.es.quadManager().selectQuad(rig.a->getId());
    rig.es.quadManager().addToSelection(rig.b->getId());
    EXPECT_EQ(rig.run(action_id::DELETE_SELECTION), Outcome::Ran);
    EXPECT_EQ(rig.quadCount(), 1u);
    rig.run(action_id::UNDO);
    EXPECT_EQ(rig.quadCount(), 3u) << "one undo step brings both back";
    rig.es.quadManager().deselectAll();
    EXPECT_EQ(rig.run(action_id::DELETE_SELECTION), Outcome::Disabled) << "nothing selected";
}

TEST(EditActionsTest, DeleteRemovesTheSelectedObjectWhenNoQuadIsSelected) {
    Rig rig;
    MeshObject* object = rig.es.activeScene().createObject("Cube");
    const UUID objectId = object->getId();
    rig.es.currentSelectionType = EditorState::SelectionType::Object;
    rig.es.selectedObjectId = objectId;

    // A quad selection is newer: the quads go, the object stays.
    rig.es.quadManager().selectQuad(rig.c->getId());
    EXPECT_EQ(rig.run(action_id::DELETE_SELECTION), Outcome::Ran);
    EXPECT_EQ(rig.quadCount(), 2u);
    EXPECT_NE(rig.es.activeScene().getObject(objectId), nullptr);

    EXPECT_EQ(rig.run(action_id::DELETE_SELECTION), Outcome::Ran);
    EXPECT_EQ(rig.es.activeScene().getObject(objectId), nullptr);
    EXPECT_TRUE(rig.es.selectedObjectId.isNull());
    EXPECT_EQ(rig.quadCount(), 2u);
}

TEST(EditActionsTest, DeleteWhileCalibratingRemovesTheSelectedPointOnly) {
    Rig rig;
    rig.es.quadManager().selectQuad(rig.a->getId());
    OutputWindow* output = rig.calibrateWithPoints(3);

    EXPECT_EQ(rig.run(action_id::DELETE_SELECTION), Outcome::Disabled) << "no point selected";
    rig.es.calibration.selected = 1;
    EXPECT_EQ(rig.run(action_id::DELETE_SELECTION), Outcome::Ran);
    EXPECT_EQ(output->getCalibrationPoints().size(), 2u);
    EXPECT_EQ(rig.quadCount(), 3u) << "never quads while calibrating";

    // Without the output manager the point cannot be reached.
    rig.ctx.outputManager = nullptr;
    EXPECT_EQ(rig.run(action_id::DELETE_SELECTION), Outcome::Disabled);
}

TEST(EditActionsTest, DeleteWhileEditingASliceWarpDoesNothing) {
    Rig rig;
    rig.es.quadManager().selectQuad(rig.a->getId());
    rig.es.sliceWarpOutputId = UUID::generate();
    EXPECT_EQ(rig.run(action_id::DELETE_SELECTION), Outcome::Disabled);
    EXPECT_FALSE(deleteSelectionAction(rig.es, &rig.om));
    EXPECT_EQ(rig.quadCount(), 3u);
}

TEST(EditActionsTest, ZOrderMovesTheSelection) {
    Rig rig;
    EXPECT_EQ(rig.run(action_id::BRING_FORWARD), Outcome::Disabled) << "nothing selected";
    rig.es.quadManager().selectQuad(rig.a->getId());
    ASSERT_EQ(rig.indexOf(rig.a), 0);
    rig.run(action_id::BRING_FORWARD);
    EXPECT_EQ(rig.indexOf(rig.a), 1);
    rig.run(action_id::BRING_TO_FRONT);
    EXPECT_EQ(rig.indexOf(rig.a), 2);
    rig.run(action_id::SEND_BACKWARD);
    EXPECT_EQ(rig.indexOf(rig.a), 1);
    rig.run(action_id::SEND_TO_BACK);
    EXPECT_EQ(rig.indexOf(rig.a), 0);

    rig.calibrateWithPoints(0);
    EXPECT_EQ(rig.run(action_id::BRING_TO_FRONT), Outcome::Disabled);
    EXPECT_EQ(rig.indexOf(rig.a), 0);
}

TEST(EditActionsTest, MeshKeysActOnThePrimarySelectedQuad) {
    Rig rig;
    EXPECT_EQ(rig.run(action_id::TOGGLE_MESH_EDIT), Outcome::Disabled) << "no quad selected";
    rig.es.quadManager().selectQuad(rig.b->getId());
    QuadGeometry& geo = rig.b->getGeometry();
    ASSERT_EQ(geo.getMeshMode(), MeshMode::CORNERS_ONLY);

    rig.run(action_id::TOGGLE_MESH_EDIT);
    EXPECT_EQ(geo.getMeshMode(), MeshMode::MESH_EDIT);

    geo.setMeshResolution(63);
    rig.run(action_id::MESH_FINER);
    EXPECT_EQ(geo.getMeshResolution(), 64);
    EXPECT_EQ(rig.run(action_id::MESH_FINER), Outcome::Disabled) << "64 is the most";
    EXPECT_EQ(geo.getMeshResolution(), 64);

    geo.setMeshResolution(3);
    rig.run(action_id::MESH_COARSER);
    EXPECT_EQ(geo.getMeshResolution(), 2);
    EXPECT_EQ(rig.run(action_id::MESH_COARSER), Outcome::Disabled) << "2 is the least";
    EXPECT_EQ(geo.getMeshResolution(), 2);

    EXPECT_EQ(rig.run(action_id::RESET_MESH), Outcome::Disabled) << "nothing to reset";
    geo.setMeshVertex(1, 1, Vec2(0.05f, 0.0f));
    ASSERT_TRUE(geo.hasMeshModifications());
    EXPECT_EQ(rig.run(action_id::RESET_MESH), Outcome::Ran);
    EXPECT_FALSE(geo.hasMeshModifications());

    // Alt+R only in mesh edit.
    geo.setMeshVertex(1, 1, Vec2(0.05f, 0.0f));
    rig.run(action_id::TOGGLE_MESH_EDIT);
    EXPECT_EQ(geo.getMeshMode(), MeshMode::CORNERS_ONLY);
    EXPECT_EQ(rig.run(action_id::RESET_MESH), Outcome::Disabled);
    EXPECT_TRUE(geo.hasMeshModifications());

    // Not while calibrating.
    rig.calibrateWithPoints(0);
    EXPECT_EQ(rig.run(action_id::TOGGLE_MESH_EDIT), Outcome::Disabled);
    EXPECT_EQ(geo.getMeshMode(), MeshMode::CORNERS_ONLY);
}

// Alt+= / Alt+- rebuild an even grid and Alt+R zeroes the mesh: each is one undo step,
// and undo puts back the grid lines, residual offsets and tangents exactly.
TEST(EditActionsTest, MeshKeysAreUndoSteps) {
    Rig rig;
    rig.es.quadManager().selectQuad(rig.b->getId());
    QuadGeometry& geo = rig.b->getGeometry();
    geo.setMeshMode(MeshMode::MESH_EDIT);
    geo.setUniformGrid(3);
    geo.insertUDivision(0.2f);  // uneven columns
    geo.setMeshVertex(1, 1, Vec2(0.05f, -0.02f));
    const int vertex = 6;  // an inner vertex: it has handles
    int handle = 0;
    while (handle < GRID_HANDLE_COUNT && !((geo.availableHandles(vertex) >> handle) & 1u)) ++handle;
    ASSERT_LT(handle, GRID_HANDLE_COUNT);
    geo.setGridHandlePosition(vertex, handle,
                              geo.gridHandlePosition(vertex, handle) + Vec2(0.03f, 0.01f));
    ASSERT_TRUE(geo.hasExplicitTangents());
    const auto u = geo.getUCoords();
    const auto v = geo.getVCoords();
    const auto offsets = geo.getMeshVertices();
    const auto tangents = geo.getVertexTangents();
    const auto expectRestored = [&] {
        EXPECT_EQ(geo.getUCoords(), u);
        EXPECT_EQ(geo.getVCoords(), v);
        EXPECT_EQ(geo.getMeshVertices(), offsets);
        EXPECT_EQ(geo.getVertexTangents(), tangents);
    };

    for (const char* id : {action_id::MESH_FINER, action_id::MESH_COARSER, action_id::RESET_MESH}) {
        ASSERT_EQ(rig.run(id), Outcome::Ran) << id;
        EXPECT_FALSE(geo.hasMeshModifications()) << id;
        EXPECT_EQ(rig.es.commandHistory.getUndoCount(), 1u) << id;
        ASSERT_EQ(rig.run(action_id::UNDO), Outcome::Ran) << id;
        expectRestored();
        rig.es.commandHistory.clear();
    }

    // A deleted quad makes the step a no-op.
    rig.run(action_id::MESH_FINER);
    rig.es.quadManager().deleteQuad(rig.b->getId());
    EXPECT_FALSE(undoAction(rig.es));
}

// A locked quad (the Quad List's lock) keeps its shape: the mesh keys are refused and
// say so; Delete leaves locked quads and says how many stayed.
TEST(EditActionsTest, LockedQuadsRefuseMeshKeysAndDelete) {
    Rig rig;
    rig.es.quadManager().selectQuad(rig.b->getId());
    QuadGeometry& geo = rig.b->getGeometry();
    geo.setMeshMode(MeshMode::MESH_EDIT);
    geo.setUniformGrid(3);
    geo.setMeshVertex(1, 1, Vec2(0.05f, 0.0f));
    rig.b->setLocked(true);
    const auto offsets = geo.getMeshVertices();

    for (const char* id : {action_id::MESH_FINER, action_id::MESH_COARSER, action_id::RESET_MESH,
                           action_id::TOGGLE_MESH_EDIT}) {
        rig.es.statusMessage[0] = '\0';
        rig.run(id);
        EXPECT_EQ(geo.getMeshResolution(), 3) << id;
        EXPECT_EQ(geo.getMeshVertices(), offsets) << id;
        EXPECT_EQ(geo.getMeshMode(), MeshMode::MESH_EDIT) << id;
        EXPECT_STREQ(rig.es.statusMessage, "B is locked (Quad List)") << id;
    }
    EXPECT_FALSE(rig.es.commandHistory.canUndo());

    // Delete: the unlocked quad goes, the locked one stays.
    rig.es.quadManager().addToSelection(rig.a->getId());
    EXPECT_EQ(rig.run(action_id::DELETE_SELECTION), Outcome::Ran);
    EXPECT_EQ(rig.quadCount(), 2u);
    EXPECT_NE(rig.es.quadManager().getQuad(rig.b->getId()), nullptr);
    EXPECT_STREQ(rig.es.statusMessage, "B is locked (Quad List): not deleted");

    // Only locked quads selected: nothing is deleted and no empty step is recorded.
    rig.c->setLocked(true);
    rig.es.quadManager().selectQuad(rig.b->getId());
    rig.es.quadManager().addToSelection(rig.c->getId());
    const size_t steps = rig.es.commandHistory.getUndoCount();
    EXPECT_FALSE(deleteSelectionAction(rig.es, &rig.om));
    EXPECT_EQ(rig.quadCount(), 2u);
    EXPECT_EQ(rig.es.commandHistory.getUndoCount(), steps);
    EXPECT_STREQ(rig.es.statusMessage, "2 locked quads were not deleted (Quad List)");
}

// The first chords are the ones the key block used; Redo also has Ctrl+Shift+Z (the
// owner's decision, 2026-09-30).
TEST(EditActionsTest, EditActionsKeepTheirChords) {
    Rig rig;
    struct Expect { const char* id; int key; uint8_t mods; };
    const Expect expected[] = {
        {action_id::UNDO, ImGuiKey_Z, MOD_CTRL},
        {action_id::REDO, ImGuiKey_Y, MOD_CTRL},
        {action_id::SELECT_ALL, ImGuiKey_A, MOD_CTRL},
        {action_id::DELETE_SELECTION, ImGuiKey_Delete, MOD_NONE},
        {action_id::BRING_FORWARD, ImGuiKey_RightBracket, MOD_CTRL},
        {action_id::SEND_BACKWARD, ImGuiKey_LeftBracket, MOD_CTRL},
        {action_id::BRING_TO_FRONT, ImGuiKey_RightBracket, MOD_CTRL | MOD_SHIFT},
        {action_id::SEND_TO_BACK, ImGuiKey_LeftBracket, MOD_CTRL | MOD_SHIFT},
        {action_id::TOGGLE_MESH_EDIT, ImGuiKey_M, MOD_CTRL},
        {action_id::MESH_FINER, ImGuiKey_Equal, MOD_ALT},
        {action_id::MESH_COARSER, ImGuiKey_Minus, MOD_ALT},
        {action_id::RESET_MESH, ImGuiKey_R, MOD_ALT},
    };
    for (const Expect& e : expected) {
        const Action* action = rig.registry.find(e.id);
        ASSERT_NE(action, nullptr) << e.id;
        ASSERT_FALSE(action->chords.empty()) << e.id;
        EXPECT_EQ(action->chords.front().key, e.key) << e.id;
        EXPECT_EQ(action->chords.front().mods, e.mods) << e.id;
        EXPECT_EQ(action->chords.size(), e.id == std::string(action_id::REDO) ? 2u : 1u) << e.id;
        EXPECT_EQ(action->cls, ActionClass::Edit) << e.id;
        EXPECT_EQ(action->guard, KeyGuard::Editing) << e.id;
    }
    EXPECT_EQ(rig.registry.find(action_id::REDO)->chords.back(),
              (Chord{ImGuiKey_Z, MOD_CTRL | MOD_SHIFT}));
    ASSERT_NE(rig.registry.find(action_id::DESELECT_ALL), nullptr);
    EXPECT_TRUE(rig.registry.find(action_id::DESELECT_ALL)->chords.empty())
        << "Esc is the perform chain, not Deselect All";
}
