// Scene takes on the live routing (ADR-047 §5): EditorState::takeScene against real
// outputs. Also slice editing on the same rig (ADR-047 part B, app/SliceEditing.hpp):
// the canvas keys, auto-blend and the canvas size, through real commands and undo.
// No GL context needed.
#include "app/EditorState.hpp"
#include "app/MeshEditKeys.hpp"
#include "app/SliceEditing.hpp"
#include <gtest/gtest.h>

using namespace quadmapper;

namespace {

struct BlendedRig {
    EditorState es;
    OutputManager om;
    OutputWindow* left = nullptr;
    OutputWindow* right = nullptr;
    OutputWindow* side = nullptr;
    UUID wide, other, next;

    BlendedRig() {
        left = om.createOutput("Left");
        right = om.createOutput("Right");
        side = om.createOutput("Side");
        Scene& first = es.activeScene();
        first.setCanvasSize(IVec2(3200, 1080));
        wide = first.getId();
        other = es.addScene("Other")->getId();
        next = es.addScene("Next")->getId();
        es.scenes[2]->setCanvasSize(IVec2(3200, 1080));
        es.setActiveScene(wide);

        // Left and right blend over one wide canvas; the side projector shows its own scene.
        OutputManager::showScene(*left, es.scenes[0].get(), es.scenes);
        OutputManager::showScene(*right, es.scenes[0].get(), es.scenes);
        OutputManager::showScene(*side, es.scenes[1].get(), es.scenes);
        left->getSlices()[0].input = CanvasRect{0.0f, 0.0f, 1920.0f, 1080.0f};
        left->getSlices()[0].blend[SLICE_EDGE_RIGHT].width = 640.0f;
        right->getSlices()[0].input = CanvasRect{1280.0f, 0.0f, 1920.0f, 1080.0f};
        right->getSlices()[0].blend[SLICE_EDGE_LEFT].width = 640.0f;
    }
};

}  // namespace

TEST(SceneTakeRoutingTest, TakeChangesTheBlendedGroupTogether) {
    BlendedRig rig;
    const SceneTake take = rig.es.takeScene(3, rig.om);

    EXPECT_EQ(take.kind, SceneTake::Kind::Route);
    EXPECT_EQ(take.outputIds, (std::vector<UUID>{rig.left->getId(), rig.right->getId()}));
    EXPECT_EQ(rig.left->getSceneId(), rig.next);
    EXPECT_EQ(rig.right->getSceneId(), rig.next);
    EXPECT_EQ(rig.side->getSceneId(), rig.other);  // not in the group: untouched
    EXPECT_EQ(rig.es.activeSceneId, rig.next);
    // The rig's slices and blends carry over (same canvas size, so unchanged).
    EXPECT_EQ(rig.right->getSlices()[0].input, (CanvasRect{1280.0f, 0.0f, 1920.0f, 1080.0f}));
    EXPECT_FLOAT_EQ(rig.right->getSlices()[0].blend[SLICE_EDGE_LEFT].width, 640.0f);

    // Back again: the whole group.
    rig.es.takeScene(1, rig.om);
    EXPECT_EQ(rig.left->getSceneId(), rig.wide);
    EXPECT_EQ(rig.right->getSceneId(), rig.wide);
    EXPECT_EQ(rig.side->getSceneId(), rig.other);
}

TEST(SceneTakeRoutingTest, TakingASceneAnotherOutputShowsOnlyActivatesIt) {
    BlendedRig rig;
    const SceneTake take = rig.es.takeScene(2, rig.om);
    EXPECT_EQ(take.kind, SceneTake::Kind::Activate);
    EXPECT_EQ(rig.es.activeSceneId, rig.other);
    EXPECT_EQ(rig.left->getSceneId(), rig.wide);
    EXPECT_EQ(rig.right->getSceneId(), rig.wide);
}

TEST(SceneTakeRoutingTest, CalibrationActivatesTheSceneTheOutputShows) {
    BlendedRig rig;
    rig.es.beginCalibration(*rig.side);
    EXPECT_EQ(rig.es.activeSceneId, rig.other);
    EXPECT_EQ(rig.es.calibratingOutputId, rig.side->getId());
}

// The active scene is on no output (just added, or clicked in the Scenes panel): the
// take still moves the whole blended group, not only its first output.
TEST(SceneTakeRoutingTest, TakeFromAnUnshownActiveSceneMovesTheWholeGroup) {
    BlendedRig rig;
    rig.es.addScene("Being edited");  // active now, on no output
    const SceneTake take = rig.es.takeScene(3, rig.om);

    EXPECT_EQ(take.kind, SceneTake::Kind::Route);
    EXPECT_EQ(take.outputIds, (std::vector<UUID>{rig.left->getId(), rig.right->getId()}));
    EXPECT_EQ(rig.left->getSceneId(), rig.next);
    EXPECT_EQ(rig.right->getSceneId(), rig.next);
    EXPECT_EQ(rig.side->getSceneId(), rig.other);
    EXPECT_EQ(rig.right->getSlices()[0].input, (CanvasRect{1280.0f, 0.0f, 1920.0f, 1080.0f}));
}

// Deleting a scene takes it off every output showing it; the next scene routed there
// gets the slices rescaled onto its own canvas.
TEST(SceneTakeRoutingTest, DeletingAShownSceneTakesItOffItsOutputs) {
    BlendedRig rig;
    ASSERT_TRUE(rig.es.deleteScene(rig.wide, &rig.om));
    EXPECT_TRUE(rig.left->getSceneId().isNull());
    EXPECT_TRUE(rig.right->getSceneId().isNull());
    EXPECT_EQ(rig.side->getSceneId(), rig.other);
    ASSERT_EQ(rig.right->getSlices().size(), 1u);  // kept for the next scene

    Scene* half = rig.es.addScene("Half");
    half->setCanvasSize(IVec2(1600, 540));
    OutputManager::showScene(*rig.right, half, rig.es.scenes);
    EXPECT_EQ(rig.right->getSlices()[0].input, (CanvasRect{640.0f, 0.0f, 960.0f, 540.0f}));
}

// A take onto a lone output through its default slice gives the scene that output's
// resolution, even when the scene was made for another projector size.
TEST(SceneTakeRoutingTest, TakeOntoALoneOutputAdoptsItsResolution) {
    EditorState es;
    OutputManager om;
    OutputWindow* out = om.createOutput("P1");  // 1920x1080
    Scene& first = es.activeScene();
    OutputManager::showScene(*out, &first, es.scenes);
    Scene* second = es.addScene("Two");
    second->setCanvasSize(out->getEffectiveResolution());  // as the Scenes panel's New does
    es.setActiveScene(first.getId());
    om.followOutputSizes(es.scenes);
    out->setResolution(IVec2(1280, 800));  // a WXGA projector
    om.followOutputSizes(es.scenes);
    ASSERT_EQ(first.getCanvasSize(), IVec2(1280, 800));

    es.takeScene(2, om);
    EXPECT_EQ(out->getSceneId(), second->getId());
    EXPECT_EQ(second->getCanvasSize(), IVec2(1280, 800));
    EXPECT_EQ(out->getSlices()[0].input, (CanvasRect{0.0f, 0.0f, 1280.0f, 800.0f}));
}

// ---- Slice editing (ADR-047 part B) ---------------------------------------------------

namespace {

CanvasKeys tabKey(bool shift = false) { CanvasKeys k; k.tab = true; k.shift = shift; return k; }
CanvasKeys rightKey(bool shift = false, bool alt = false) {
    CanvasKeys k; k.right = true; k.shift = shift; k.alt = alt; return k;
}

// The blended rig with the slice resolver wired as Application wires it, and a
// selected quad on the wide scene.
struct SliceRig : BlendedRig {
    Quad* quad = nullptr;
    SliceRig() {
        es.outputResolver.setOutputManager(&om);
        quad = es.quadManager().createQuad();
        es.quadManager().selectQuad(quad->getId());
    }
    // One frame of canvas keys, dispatched as ViewportEditor does.
    bool press(const CanvasKeys& keys) {
        switch (canvasKeyOwner(es, &om)) {
        case CanvasKeyOwner::Quad:
            return applyCornerKeys(keys, es.quadRef(*quad), quad->getGeometry(), es.commandHistory,
                                   es.cornerSelectedVertex);
        case CanvasKeyOwner::SliceWarp:
            return applySliceWarpKeys(keys, es, *sliceWarpOutput(es, &om));
        default:
            return false;
        }
    }
};

}  // namespace

// The success metric stays the quad's while a slice is selected in the slice editor
// but its warp is not being edited: Tab / Shift / Alt / the arrows move the quad's
// corner, never a slice.
TEST(SliceEditingTest, QuadKeysAreUnaffectedWhileTheSliceEditorIsNotActive) {
    SliceRig rig;
    rig.es.currentSelectionType = EditorState::SelectionType::Output;
    rig.es.selectedSliceId = rig.left->getSlices()[0].id;  // slice editor open on it
    ASSERT_EQ(canvasKeyOwner(rig.es, &rig.om), CanvasKeyOwner::Quad);

    const Vec2 tl = rig.quad->getGeometry().getCorners()[0];
    EXPECT_FALSE(rig.press(tabKey()));                  // selects TL
    EXPECT_EQ(rig.es.cornerSelectedVertex, 0);
    EXPECT_TRUE(rig.press(rightKey(/*shift=*/true)));   // 10x
    EXPECT_TRUE(rig.press(rightKey(false, /*alt=*/true)));  // 0.1x
    EXPECT_NEAR(rig.quad->getGeometry().getCorners()[0].x, tl.x + 0.0101f, 1e-6f);
    EXPECT_TRUE(isIdentityWarp(rig.left->getSlices()[0].warp));
    EXPECT_TRUE(isIdentityWarp(rig.right->getSlices()[0].warp));
    EXPECT_EQ(rig.es.sliceWarpSelectedCorner, -1);
    ASSERT_TRUE(rig.es.commandHistory.undo());          // the nudges were one quad step
    EXPECT_EQ(rig.quad->getGeometry().getCorners()[0], tl);
}

// Turned on in the slice editor, warp editing takes the canvas keys with the quad's
// model (Tab cycles, arrows nudge, Shift 10x, Alt 0.1x), one undo step per corner;
// turned off, the quad has them back.
TEST(SliceEditingTest, SliceWarpTakesTheCanvasKeysOnlyWhileOn) {
    SliceRig rig;
    const UUID sliceId = rig.left->getSlices()[0].id;
    rig.es.beginSliceWarp(*rig.left, sliceId);
    ASSERT_EQ(canvasKeyOwner(rig.es, &rig.om), CanvasKeyOwner::SliceWarp);
    const Vec2 quadTl = rig.quad->getGeometry().getCorners()[0];

    EXPECT_FALSE(rig.press(tabKey(/*shift=*/true)));  // Shift+Tab from none: BL
    EXPECT_EQ(rig.es.sliceWarpSelectedCorner, 3);
    EXPECT_FALSE(rig.press(tabKey()));                // wraps to TL
    EXPECT_EQ(rig.es.sliceWarpSelectedCorner, 0);
    EXPECT_TRUE(rig.press(rightKey()));
    EXPECT_TRUE(rig.press(rightKey(true)));
    EXPECT_TRUE(rig.press(rightKey(false, true)));
    EXPECT_NEAR(rig.left->getSlices()[0].warp.getCorners()[0].x, 0.0111f, 1e-6f);
    EXPECT_EQ(rig.quad->getGeometry().getCorners()[0], quadTl) << "the quad never moves";
    EXPECT_EQ(rig.es.commandHistory.getUndoCount(), 1u) << "nudges of one corner merge";

    EXPECT_FALSE(rig.press(tabKey()));                // TR: a new undo step
    EXPECT_TRUE(rig.press(rightKey()));
    EXPECT_EQ(rig.es.commandHistory.getUndoCount(), 2u);

    ASSERT_TRUE(rig.es.commandHistory.undo());
    ASSERT_TRUE(rig.es.commandHistory.undo());
    EXPECT_TRUE(isIdentityWarp(rig.left->getSlices()[0].warp));

    rig.es.endSliceWarp();
    EXPECT_EQ(canvasKeyOwner(rig.es, &rig.om), CanvasKeyOwner::Quad);
}

// Warp editing whose slice is gone (removed, undone) ends by itself, and the canvas
// keys go back to the quad.
TEST(SliceEditingTest, WarpEditingOfAVanishedSliceEnds) {
    SliceRig rig;
    rig.es.beginSliceWarp(*rig.left, rig.left->getSlices()[0].id);
    editSlices(rig.es, *rig.left, "Remove slice", "",
               [](std::vector<Slice>& s) { s[0] = makeFullCanvasSlice(IVec2(3200, 1080)); });
    EXPECT_EQ(canvasKeyOwner(rig.es, &rig.om), CanvasKeyOwner::Quad);
    EXPECT_TRUE(rig.es.sliceWarpOutputId.isNull());
}

// Calibration needs the untouched default slice (ADR-047 Amendment 3), and entering
// it leaves warp editing.
TEST(SliceEditingTest, CalibrationOnlyWithTheDefaultSlicing) {
    SliceRig rig;
    EXPECT_FALSE(rig.es.canCalibrate(*rig.left));  // blended slice
    rig.es.beginCalibration(*rig.left);
    EXPECT_TRUE(rig.es.calibratingOutputId.isNull());

    EXPECT_TRUE(rig.es.canCalibrate(*rig.side));   // one whole-canvas slice
    rig.es.beginSliceWarp(*rig.left, rig.left->getSlices()[0].id);
    rig.es.beginCalibration(*rig.side);
    EXPECT_EQ(rig.es.calibratingOutputId, rig.side->getId());
    EXPECT_TRUE(rig.es.sliceWarpOutputId.isNull());
    rig.es.beginSliceWarp(*rig.left, rig.left->getSlices()[0].id);
    EXPECT_TRUE(rig.es.sliceWarpOutputId.isNull()) << "not while calibrating";
}

TEST(SliceEditingTest, AutoBlendSetsTheFacingEdgesAsOneUndoStep) {
    SliceRig rig;
    for (OutputWindow* o : {rig.left, rig.right}) {
        for (BlendEdge& e : o->getSlices()[0].blend) e = BlendEdge{};
    }
    rig.right->getSlices()[0].blend[SLICE_EDGE_TOP].width = 7.0f;  // not facing: kept

    EXPECT_EQ(autoBlendScene(rig.es, rig.om, rig.wide), 2);
    const Slice& l = rig.left->getSlices()[0];
    const Slice& r = rig.right->getSlices()[0];
    EXPECT_FLOAT_EQ(l.blend[SLICE_EDGE_RIGHT].width, 640.0f);
    EXPECT_FLOAT_EQ(l.blend[SLICE_EDGE_RIGHT].gamma, 2.2f);
    EXPECT_FLOAT_EQ(l.blend[SLICE_EDGE_RIGHT].curve, 2.0f);
    EXPECT_FLOAT_EQ(r.blend[SLICE_EDGE_LEFT].width, 640.0f);
    EXPECT_FLOAT_EQ(r.blend[SLICE_EDGE_TOP].width, 7.0f);
    EXPECT_FLOAT_EQ(l.blend[SLICE_EDGE_LEFT].width, 0.0f);
    EXPECT_EQ(rig.es.commandHistory.getUndoCount(), 1u);

    ASSERT_TRUE(rig.es.commandHistory.undo());
    EXPECT_FLOAT_EQ(rig.left->getSlices()[0].blend[SLICE_EDGE_RIGHT].width, 0.0f);
    EXPECT_FLOAT_EQ(rig.right->getSlices()[0].blend[SLICE_EDGE_LEFT].width, 0.0f);

    // A scene one output shows has nothing to blend: no step.
    EXPECT_EQ(autoBlendScene(rig.es, rig.om, rig.other), 0);
    EXPECT_FALSE(rig.es.commandHistory.canUndo());
}

// Canvas size from the Scenes panel: the slices of every output showing the scene
// keep their place on the picture, the size is fixed against adoption, and one undo
// puts everything back.
TEST(SliceEditingTest, CanvasResizeRescalesSlicesLocksAndUndoes) {
    SliceRig rig;
    const Vec2 quadCorner = rig.quad->getGeometry().getCorners()[2];
    ASSERT_TRUE(setSceneCanvas(rig.es, &rig.om, rig.wide, IVec2(6400, 2160), true));
    Scene& wide = *rig.es.scenes[0];
    EXPECT_EQ(wide.getCanvasSize(), IVec2(6400, 2160));
    EXPECT_TRUE(wide.isCanvasLocked());
    EXPECT_EQ(rig.right->getSlices()[0].input, (CanvasRect{2560.0f, 0.0f, 3840.0f, 2160.0f}));
    EXPECT_FLOAT_EQ(rig.right->getSlices()[0].blend[SLICE_EDGE_LEFT].width, 1280.0f);
    EXPECT_EQ(rig.right->getSlicesCanvas(), IVec2(6400, 2160));
    EXPECT_EQ(rig.side->getSlices()[0].input, (CanvasRect{0.0f, 0.0f, 1920.0f, 1080.0f}))
        << "another scene's output is untouched";
    EXPECT_EQ(rig.quad->getGeometry().getCorners()[2], quadCorner);
    EXPECT_EQ(rig.es.commandHistory.getUndoCount(), 1u);

    // Routing keeps a fixed canvas: the side projector alone on it adopts nothing.
    rig.om.routeScene(*rig.left, nullptr, rig.es.scenes);
    rig.om.routeScene(*rig.right, nullptr, rig.es.scenes);
    rig.om.routeScene(*rig.side, &wide, rig.es.scenes);
    EXPECT_EQ(wide.getCanvasSize(), IVec2(6400, 2160));
    rig.om.routeScene(*rig.side, rig.es.scenes[1].get(), rig.es.scenes);
    rig.om.routeScene(*rig.left, &wide, rig.es.scenes);
    rig.om.routeScene(*rig.right, &wide, rig.es.scenes);

    ASSERT_TRUE(rig.es.commandHistory.undo());
    EXPECT_EQ(wide.getCanvasSize(), IVec2(3200, 1080));
    EXPECT_FALSE(wide.isCanvasLocked());
    EXPECT_EQ(rig.right->getSlices()[0].input, (CanvasRect{1280.0f, 0.0f, 1920.0f, 1080.0f}));
    EXPECT_EQ(rig.right->getSlicesCanvas(), IVec2(3200, 1080));
    ASSERT_TRUE(rig.es.commandHistory.redo());
    EXPECT_EQ(rig.right->getSlices()[0].input, (CanvasRect{2560.0f, 0.0f, 3840.0f, 2160.0f}));

    // Clamped like the loader; no change, no step.
    ASSERT_TRUE(setSceneCanvas(rig.es, &rig.om, rig.wide, IVec2(100000, 0), true));
    EXPECT_EQ(wide.getCanvasSize(), IVec2(MAX_CANVAS_DIMENSION, 1));
    const size_t steps = rig.es.commandHistory.getUndoCount();
    EXPECT_FALSE(setSceneCanvas(rig.es, &rig.om, rig.wide, IVec2(MAX_CANVAS_DIMENSION, 1), true));
    EXPECT_EQ(rig.es.commandHistory.getUndoCount(), steps);
    // Unlocking alone is a step too.
    EXPECT_TRUE(setSceneCanvas(rig.es, &rig.om, rig.wide, wide.getCanvasSize(), false));
    EXPECT_FALSE(wide.isCanvasLocked());
}
