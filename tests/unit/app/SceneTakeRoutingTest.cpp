// Scene takes on the live routing (ADR-047 §5): EditorState::takeScene against real
// outputs, with and without crossfades. Also slice editing on the same rig (ADR-047
// part B, app/SliceEditing.hpp): the canvas keys, auto-blend and the canvas size,
// through real commands and undo. And what selecting a quad shows in the Properties
// panel (app/QuadSelection.hpp). No GL context needed.
#include "app/EditorState.hpp"
#include "app/MeshEditKeys.hpp"
#include "app/ProjectReplace.hpp"
#include "app/QuadSelection.hpp"
#include "app/SliceEditing.hpp"
#include "app/UndoRouting.hpp"
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

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
    EXPECT_FALSE(rig.press(tabKey(/*shift=*/true)));    // Shift+Tab from none: BL
    EXPECT_EQ(rig.es.cornerSelectedVertex, 3);
    EXPECT_FALSE(rig.press(tabKey(/*shift=*/true)));    // backwards: BR
    EXPECT_EQ(rig.es.cornerSelectedVertex, 2);
    EXPECT_FALSE(rig.press(tabKey()));                  // forwards: BL
    EXPECT_FALSE(rig.press(tabKey()));                  // wraps to TL
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

// Calibration works on an output however it is sliced (TD-29): the blended and warped
// output enters it, the canvas shows that output's frame (calibrationViewOutput) and
// the canvas keys go to its points, never the quad. Entering it leaves warp editing,
// and warp editing cannot start while calibrating.
TEST(SliceEditingTest, CalibrationWorksOnAnySlicing) {
    SliceRig rig;
    rig.right->getSlices()[0].warp.setCorners({Vec2(0.0f, 0.0f), Vec2(1.0f, 0.06f),
                                               Vec2(0.97f, 0.95f), Vec2(0.02f, 1.0f)});
    ASSERT_FALSE(isDefaultSlicing(rig.right->getSlices(), IVec2(3200, 1080)));
    rig.es.beginSliceWarp(*rig.left, rig.left->getSlices()[0].id);

    rig.es.beginCalibration(*rig.right);  // blended, keystoned, part of the canvas
    EXPECT_EQ(rig.es.calibratingOutputId, rig.right->getId());
    EXPECT_TRUE(rig.es.sliceWarpOutputId.isNull());
    EXPECT_EQ(calibrationViewOutput(rig.es, &rig.om), rig.right);
    EXPECT_EQ(canvasKeyOwner(rig.es, &rig.om), CanvasKeyOwner::None) << "no points yet";
    rig.right->getCalibrationPoints().push_back(CalibrationPoint{});
    EXPECT_EQ(canvasKeyOwner(rig.es, &rig.om), CanvasKeyOwner::Calibration);
    const Vec2 quadTl = rig.quad->getGeometry().getCorners()[0];
    EXPECT_FALSE(rig.press(tabKey()));
    EXPECT_FALSE(rig.press(rightKey(/*shift=*/true)));
    EXPECT_EQ(rig.quad->getGeometry().getCorners()[0], quadTl) << "the quad never moves";

    rig.es.beginSliceWarp(*rig.left, rig.left->getSlices()[0].id);
    EXPECT_TRUE(rig.es.sliceWarpOutputId.isNull()) << "not while calibrating";

    // Another scene made active: the canvas is that scene's, and no point is edited on it.
    rig.es.setActiveScene(rig.other);
    EXPECT_EQ(calibrationViewOutput(rig.es, &rig.om), nullptr);
    EXPECT_EQ(canvasKeyOwner(rig.es, &rig.om), CanvasKeyOwner::None);

    rig.es.endCalibration();
    EXPECT_EQ(calibrationViewOutput(rig.es, &rig.om), nullptr);
}

namespace {

// A 2x2x2 box seen obliquely by a projector of `size` (as in CalibrationSessionTest).
ProjectorCamera trueProjector(IVec2 size) {
    return orbitCamera(Vec3(0.0f), 25.0f, 15.0f, 6.0f, size.x, size.y);
}

// Calibrate `output` from the box's eight corners as the operator would on the
// calibration view: each point is picked where the view shows it (the output frame, in
// which the truth projects it; CalibrationViewTest proves the view draws it there
// whatever the slicing) and mapped by canvasToPixel. Leaves calibration running.
void pickBoxCorners(EditorState& es, OutputWindow& output, const ProjectorCamera& truth) {
    output.getCalibrationPoints().clear();
    output.getProjector() = ProjectorCamera{};
    output.getProjector().intrinsics = ProjectorIntrinsics::fromThrowRatio(
        1.5f, truth.intrinsics.width, truth.intrinsics.height);
    es.beginCalibration(output);
    const ProjectorIntrinsics& k = output.getProjector().intrinsics;
    for (int i = 0; i < 8; ++i) {
        CalibrationPoint point;
        point.world = Vec3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
        Vec2 truthPixel;
        ASSERT_TRUE(truth.project(point.world, truthPixel));
        const Vec2 onView(truthPixel.x / static_cast<float>(k.width),
                          truthPixel.y / static_cast<float>(k.height));
        point.pixel = canvasToPixel(onView, k);
        output.getCalibrationPoints().push_back(point);
    }
    es.calibration.markChanged();
    ASSERT_TRUE(es.calibration.update(output.getCalibrationPoints(), output.getProjector()))
        << es.calibration.status;
}

}  // namespace

// A solve on a warped, blended slice of the canvas recovers the same camera as one on
// an output with the default slicing, and the true camera, within the solver's usual
// tolerances (CalibrationSessionTest.EnoughPointsSolveAndCalibrateTheProjector).
TEST(SliceEditingTest, ASolveOnAWarpedSliceRecoversTheCameraOfTheDefaultSlicing) {
    SliceRig rig;
    rig.right->getSlices()[0].warp.setCorners({Vec2(0.0f, 0.0f), Vec2(1.0f, 0.06f),
                                               Vec2(0.97f, 0.95f), Vec2(0.02f, 1.0f)});
    ASSERT_TRUE(isDefaultSlicing(rig.side->getSlices(), rig.es.scenes[1]->getCanvasSize()));
    const IVec2 size = rig.right->getEffectiveResolution();
    ASSERT_EQ(size, rig.side->getEffectiveResolution());
    const ProjectorCamera truth = trueProjector(size);

    pickBoxCorners(rig.es, *rig.side, truth);
    const ProjectorCamera plain = rig.side->getProjector();
    rig.es.endCalibration();
    pickBoxCorners(rig.es, *rig.right, truth);
    const ProjectorCamera warped = rig.right->getProjector();
    EXPECT_LT(rig.es.calibration.rmsError, 0.05f);
    rig.es.endCalibration();

    ASSERT_TRUE(plain.calibrated);
    ASSERT_TRUE(warped.calibrated);
    for (const ProjectorCamera* solved : {&plain, &warped}) {
        EXPECT_NEAR(solved->intrinsics.fx, truth.intrinsics.fx, 1.0f);
        EXPECT_NEAR(solved->intrinsics.cx, truth.intrinsics.cx, 1.0f);
        EXPECT_NEAR(solved->intrinsics.cy, truth.intrinsics.cy, 1.0f);
        EXPECT_LT(glm::length(solved->position() - truth.position()), 1e-2f);
    }
    EXPECT_NEAR(warped.intrinsics.fx, plain.intrinsics.fx, 1e-3f);
    EXPECT_LT(glm::length(warped.position() - plain.position()), 1e-4f);
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
        Vec2 a, b;
        ASSERT_TRUE(plain.project(corner, a));
        ASSERT_TRUE(warped.project(corner, b));
        EXPECT_LT(glm::length(a - b), 0.01f);
    }
}

// A slice change while its output is calibrated (the slice editor, auto-blend, a canvas
// resize) changes only the 2D picture the calibration view shows: the points are
// projector pixels and the objects are drawn from the projector, so the points, the
// solved projector and the session stay as they were, with no re-solve. The slice steps
// are held back from undo until calibration ends (TD-17 scoping), then undo in order.
TEST(SliceEditingTest, ASliceChangeMidCalibrationLeavesThePointsAndTheSolve) {
    SliceRig rig;
    const ProjectorCamera truth = trueProjector(rig.right->getEffectiveResolution());
    pickBoxCorners(rig.es, *rig.right, truth);
    ASSERT_EQ(rig.es.calibratingOutputId, rig.right->getId());
    const std::vector<CalibrationPoint> points = rig.right->getCalibrationPoints();
    const ProjectorCamera solved = rig.right->getProjector();
    const std::vector<Slice> slicesBefore = rig.right->getSlices();
    ASSERT_FALSE(rig.es.calibration.isDirty());

    const UUID sliceId = rig.right->getSlices()[0].id;
    editSlices(rig.es, *rig.right, "Move slice warp corner", sliceWarpCornerKey(sliceId, 1),
               [](std::vector<Slice>& s) { s[0].warp.setCorner(1, Vec2(0.9f, 0.1f)); });
    rig.right->getSlices()[0].blend[SLICE_EDGE_LEFT].width = 0.0f;
    rig.left->getSlices()[0].blend[SLICE_EDGE_RIGHT].width = 0.0f;
    EXPECT_EQ(autoBlendScene(rig.es, rig.om, rig.wide), 2) << "the calibrated output blends too";
    EXPECT_FLOAT_EQ(rig.right->getSlices()[0].blend[SLICE_EDGE_LEFT].width, 640.0f);
    EXPECT_TRUE(setSceneCanvas(rig.es, rig.wide, IVec2(1600, 540), /*locked=*/true));
    EXPECT_FLOAT_EQ(rig.right->getSlices()[0].input.x, 640.0f) << "rescaled with the canvas";

    ASSERT_EQ(rig.right->getCalibrationPoints().size(), points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        EXPECT_EQ(rig.right->getCalibrationPoints()[i].pixel, points[i].pixel);
        EXPECT_EQ(rig.right->getCalibrationPoints()[i].world, points[i].world);
    }
    EXPECT_EQ(rig.right->getProjector().pose.rotation, solved.pose.rotation);
    EXPECT_EQ(rig.right->getProjector().pose.translation, solved.pose.translation);
    EXPECT_EQ(rig.right->getProjector().intrinsics.fx, solved.intrinsics.fx);
    EXPECT_FALSE(rig.es.calibration.isDirty()) << "no re-solve";
    EXPECT_FALSE(rig.es.calibration.update(rig.right->getCalibrationPoints(), rig.right->getProjector()));
    EXPECT_EQ(calibrationViewOutput(rig.es, &rig.om), rig.right);
    EXPECT_EQ(canvasKeyOwner(rig.es, &rig.om), CanvasKeyOwner::Calibration);

    std::string heldBack;
    EXPECT_FALSE(undoStep(rig.es, &heldBack));
    EXPECT_FALSE(heldBack.empty());
    EXPECT_STREQ(undoScopeName(rig.es), "calibration");
    rig.es.endCalibration();
    ASSERT_TRUE(undoStep(rig.es));  // the canvas size
    ASSERT_TRUE(undoStep(rig.es));  // auto-blend
    ASSERT_TRUE(undoStep(rig.es));  // the warp corner
    EXPECT_EQ(rig.right->getSlices()[0].warp.getCorners(), slicesBefore[0].warp.getCorners());
    EXPECT_EQ(rig.right->getCalibrationPoints().size(), points.size()) << "slice undo leaves points";
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
    ASSERT_TRUE(setSceneCanvas(rig.es, rig.wide, IVec2(6400, 2160), true));
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
    ASSERT_TRUE(setSceneCanvas(rig.es, rig.wide, IVec2(100000, 0), true));
    EXPECT_EQ(wide.getCanvasSize(), IVec2(MAX_CANVAS_DIMENSION, 1));
    const size_t steps = rig.es.commandHistory.getUndoCount();
    EXPECT_FALSE(setSceneCanvas(rig.es, rig.wide, IVec2(MAX_CANVAS_DIMENSION, 1), true));
    EXPECT_EQ(rig.es.commandHistory.getUndoCount(), steps);
    // Unlocking alone is a step too.
    EXPECT_TRUE(setSceneCanvas(rig.es, rig.wide, wide.getCanvasSize(), false));
    EXPECT_FALSE(wide.isCanvasLocked());
}

namespace {

// A scene of half the wide canvas's size, taken as key 4.
UUID addHalfScene(SliceRig& rig) {
    Scene* half = rig.es.addScene("Half");
    half->setCanvasSize(IVec2(1600, 540));
    rig.es.setActiveScene(rig.wide);
    return half->getId();
}

}  // namespace

// Takes rescale slices without an undo step (ADR-047 Amendment 5): undoing and redoing
// an edit made on the old canvas keeps the slice where the take put it on the new one.
TEST(SliceEditingTest, UndoAfterATakeToAnotherCanvasFitsTheCanvasShownNow) {
    SliceRig rig;
    const UUID half = addHalfScene(rig);
    editSlices(rig.es, *rig.right, "Black level", "black",
               [](std::vector<Slice>& s) { s[0].blackLevel = 0.1f; });
    rig.es.takeScene(4, rig.om);
    ASSERT_EQ(rig.right->getSceneId(), half);
    const CanvasRect onHalf{640.0f, 0.0f, 960.0f, 540.0f};
    ASSERT_EQ(rig.right->getSlices()[0].input, onHalf);

    ASSERT_TRUE(rig.es.commandHistory.undo());
    EXPECT_FLOAT_EQ(rig.right->getSlices()[0].blackLevel, 0.0f);
    EXPECT_EQ(rig.right->getSlices()[0].input, onHalf);
    EXPECT_FLOAT_EQ(rig.right->getSlices()[0].blend[SLICE_EDGE_LEFT].width, 320.0f);
    EXPECT_EQ(rig.right->getSlicesCanvas(), IVec2(1600, 540));
    ASSERT_TRUE(rig.es.commandHistory.redo());
    EXPECT_FLOAT_EQ(rig.right->getSlices()[0].blackLevel, 0.1f);
    EXPECT_EQ(rig.right->getSlices()[0].input, onHalf);
}

// Nudges of one corner before and after a take are two steps, each undone onto the
// canvas shown now.
TEST(SliceEditingTest, NudgesDoNotMergeAcrossATake) {
    SliceRig rig;
    addHalfScene(rig);
    rig.es.beginSliceWarp(*rig.right, rig.right->getSlices()[0].id);
    applySliceWarpKeys(tabKey(), rig.es, *rig.right);
    ASSERT_TRUE(applySliceWarpKeys(rightKey(), rig.es, *rig.right));
    rig.es.takeScene(4, rig.om);
    ASSERT_TRUE(applySliceWarpKeys(rightKey(), rig.es, *rig.right));
    EXPECT_EQ(rig.es.commandHistory.getUndoCount(), 2u);

    ASSERT_TRUE(rig.es.commandHistory.undo());
    EXPECT_NEAR(rig.right->getSlices()[0].warp.getCorners()[0].x, 0.001f, 1e-6f);
    ASSERT_TRUE(rig.es.commandHistory.undo());
    EXPECT_TRUE(isIdentityWarp(rig.right->getSlices()[0].warp));
    EXPECT_EQ(rig.right->getSlices()[0].input, (CanvasRect{640.0f, 0.0f, 960.0f, 540.0f}));
}

// Undoing a canvas resize rescales every output showing the scene at that moment,
// including one routed to it after the resize.
TEST(SliceEditingTest, UndoOfACanvasResizeRescalesOutputsRoutedSince) {
    SliceRig rig;
    Scene& wide = *rig.es.scenes[0];
    ASSERT_TRUE(setSceneCanvas(rig.es, rig.wide, IVec2(6400, 2160), true));
    rig.om.routeScene(*rig.side, &wide, rig.es.scenes);
    ASSERT_EQ(rig.side->getSlices()[0].input, (CanvasRect{0.0f, 0.0f, 6400.0f, 2160.0f}));

    ASSERT_TRUE(rig.es.commandHistory.undo());
    EXPECT_EQ(rig.side->getSlices()[0].input, (CanvasRect{0.0f, 0.0f, 3200.0f, 1080.0f}));
    EXPECT_EQ(rig.side->getSlicesCanvas(), IVec2(3200, 1080));
    EXPECT_EQ(rig.right->getSlices()[0].input, (CanvasRect{1280.0f, 0.0f, 1920.0f, 1080.0f}));
    ASSERT_TRUE(rig.es.commandHistory.redo());
    EXPECT_EQ(rig.side->getSlices()[0].input, (CanvasRect{0.0f, 0.0f, 6400.0f, 2160.0f}));
    EXPECT_EQ(rig.side->getSlicesCanvas(), IVec2(6400, 2160));
}

// Esc (or the slice editor's button) mid-drag leaves the corner where the operator put
// it, as one undo step, as the mouse release would have.
TEST(SliceEditingTest, LeavingWarpEditingMidDragRecordsTheDrag) {
    SliceRig rig;
    rig.es.beginSliceWarp(*rig.right, rig.right->getSlices()[0].id);
    // The press and the held drag, as ViewportEditor's editSliceWarp does them.
    rig.es.sliceWarpDraggedCorner = rig.es.sliceWarpSelectedCorner = 0;
    rig.es.sliceWarpDragStartPos = rig.right->getSlices()[0].warp.getCorners()[0];
    rig.es.sliceWarpDragBefore = OutputSlices{rig.right->getSlices(), rig.right->getSlicesCanvas()};
    rig.right->getSlices()[0].warp.setCorner(0, Vec2(0.2f, 0.2f));

    rig.es.endSliceWarp();
    EXPECT_EQ(rig.right->getSlices()[0].warp.getCorners()[0], Vec2(0.2f, 0.2f));
    EXPECT_EQ(rig.es.sliceWarpDraggedCorner, -1);
    ASSERT_TRUE(rig.es.commandHistory.canUndo());
    EXPECT_EQ(rig.es.commandHistory.getUndoDescription(), "Move slice warp corner");
    ASSERT_TRUE(rig.es.commandHistory.undo());
    EXPECT_TRUE(isIdentityWarp(rig.right->getSlices()[0].warp));
    // Leaving with no drag records nothing.
    rig.es.beginSliceWarp(*rig.right, rig.right->getSlices()[0].id);
    rig.es.endSliceWarp();
    EXPECT_FALSE(rig.es.commandHistory.canUndo());
}

// While a slice warp is edited, Ctrl+Z / Ctrl+Y take only that output's slice steps: a
// quad edit hidden behind the warp view, or another output's slices, are held back.
TEST(SliceEditingTest, WarpEditingUndoesOnlyThatOutputsSliceSteps) {
    SliceRig rig;
    const Vec2 quadTl = rig.quad->getGeometry().getCorners()[0];
    rig.press(tabKey());
    ASSERT_TRUE(rig.press(rightKey()));  // a quad step
    editSlices(rig.es, *rig.right, "Black level", "black",
               [](std::vector<Slice>& s) { s[0].blackLevel = 0.1f; });
    editSlices(rig.es, *rig.left, "Black level", "black",
               [](std::vector<Slice>& s) { s[0].blackLevel = 0.2f; });

    rig.es.beginSliceWarp(*rig.left, rig.left->getSlices()[0].id);
    EXPECT_TRUE(canUndoStep(rig.es));
    ASSERT_TRUE(undoStep(rig.es));
    EXPECT_FLOAT_EQ(rig.left->getSlices()[0].blackLevel, 0.0f);
    std::string heldBack;
    EXPECT_FALSE(canUndoStep(rig.es));
    EXPECT_FALSE(undoStep(rig.es, &heldBack));
    EXPECT_EQ(heldBack, "Black level");
    EXPECT_FLOAT_EQ(rig.right->getSlices()[0].blackLevel, 0.1f);
    EXPECT_STREQ(undoScopeName(rig.es), "slice warp editing");

    rig.es.endSliceWarp();
    ASSERT_TRUE(undoStep(rig.es));  // the right output's step
    ASSERT_TRUE(undoStep(rig.es));  // the quad's
    EXPECT_EQ(rig.quad->getGeometry().getCorners()[0], quadTl);

    rig.es.beginSliceWarp(*rig.left, rig.left->getSlices()[0].id);
    EXPECT_FALSE(canRedoStep(rig.es)) << "the quad step is next to redo";
    EXPECT_FALSE(redoStep(rig.es));
    EXPECT_EQ(rig.quad->getGeometry().getCorners()[0], quadTl);
}

// The slice editor's input rectangle stays on the canvas.
TEST(SliceEditingTest, InputRectIsClampedToTheCanvas) {
    const IVec2 canvas(3200, 1080);
    EXPECT_EQ(clampInputToCanvas(CanvasRect{1280.0f, 0.0f, 1920.0f, 1080.0f}, canvas),
              (CanvasRect{1280.0f, 0.0f, 1920.0f, 1080.0f}));
    EXPECT_EQ(clampInputToCanvas(CanvasRect{-10.0f, -5.0f, 5000.0f, 2000.0f}, canvas),
              (CanvasRect{0.0f, 0.0f, 3200.0f, 1080.0f}));
    EXPECT_EQ(clampInputToCanvas(CanvasRect{2800.0f, 0.0f, 1920.0f, 1080.0f}, canvas),
              (CanvasRect{2800.0f, 0.0f, 400.0f, 1080.0f}));
    EXPECT_EQ(clampInputToCanvas(CanvasRect{3500.0f, 2000.0f, 0.0f, -3.0f}, canvas),
              (CanvasRect{3199.0f, 1079.0f, 1.0f, 1.0f}));
}

// File > New / Open / recovery: the next show's projectors show the show, not the
// test pattern or a fade from the old show, and no slice editing carries over.
TEST(SliceEditingTest, ProjectReplaceEndsSliceEditingTheTestPatternAndFades) {
    SliceRig rig;
    rig.es.beginSliceWarp(*rig.left, rig.left->getSlices()[0].id);
    rig.om.setBlendTestPattern(true);
    rig.om.setFrameTime(10.0);
    rig.es.takeScene(3, rig.om, 1.0);
    ASSERT_FALSE(rig.om.sceneFades().empty());

    clearProjectContent(rig.es, nullptr, &rig.om);
    EXPECT_TRUE(rig.es.sliceWarpOutputId.isNull());
    EXPECT_TRUE(rig.es.selectedSliceId.isNull());
    EXPECT_FALSE(rig.om.isBlendTestPattern());
    EXPECT_TRUE(rig.om.sceneFades().empty());
}

// ---- Crossfaded takes (ADR-047 §5, QM-121) ------------------------------------------

// A take with a fade fades the whole blended group together, from the scene it showed,
// on the frame clock; the side projector is untouched. The fade ends on time.
TEST(SceneTakeFadeTest, TheBlendedGroupFadesTogether) {
    BlendedRig rig;
    rig.om.setFrameTime(10.0);
    const SceneTake take = rig.es.takeScene(3, rig.om, 1.0);
    ASSERT_EQ(take.kind, SceneTake::Kind::Route);
    EXPECT_EQ(rig.left->getSceneId(), rig.next) << "the routing changes at once";
    EXPECT_EQ(rig.es.activeSceneId, rig.next);
    for (OutputWindow* o : {rig.left, rig.right}) {
        const SceneFade* fade = rig.om.sceneFade(*o);
        ASSERT_NE(fade, nullptr) << o->getName();
        EXPECT_EQ(fade->fromSceneId, rig.wide);
        EXPECT_EQ(fade->toSceneId, rig.next);
        EXPECT_FLOAT_EQ(sceneFadeMix(*fade, rig.om.frameTime()), 0.0f);
    }
    EXPECT_EQ(rig.om.sceneFade(*rig.side), nullptr);

    rig.om.setFrameTime(10.5);
    EXPECT_FALSE(rig.om.retireSceneFades(rig.es.scenes));
    EXPECT_FLOAT_EQ(rig.om.sceneFades().mix(rig.left->getId(), 10.5), 0.5f);
    EXPECT_FLOAT_EQ(rig.om.sceneFades().mix(rig.right->getId(), 10.5), 0.5f);

    rig.om.setFrameTime(11.0);
    EXPECT_TRUE(rig.om.retireSceneFades(rig.es.scenes));
    EXPECT_TRUE(rig.om.sceneFades().empty());
    EXPECT_EQ(rig.left->getSceneId(), rig.next);
}

// Gate (ADR-047 §8): a fade of 0 is today's cut — same routing, no fade state.
TEST(SceneTakeFadeTest, AZeroFadeIsACut) {
    BlendedRig cut;
    BlendedRig faded;
    cut.es.takeScene(3, cut.om);
    faded.es.takeScene(3, faded.om, 0.0);
    EXPECT_TRUE(faded.om.sceneFades().empty());
    EXPECT_EQ(cut.left->getSceneId(), cut.next);
    EXPECT_EQ(faded.left->getSceneId(), faded.next);
    EXPECT_EQ(faded.right->getSceneId(), faded.next);
    EXPECT_EQ(faded.right->getSlices()[0].input, cut.right->getSlices()[0].input);
    EXPECT_EQ(faded.om.sceneFade(*faded.left), nullptr);
}

// Taking back the scene that is fading out reverses the group's fade from where it is.
TEST(SceneTakeFadeTest, ATakeDuringAFadeStartsFromWhatIsOnScreen) {
    BlendedRig rig;
    rig.om.setFrameTime(0.0);
    rig.es.takeScene(3, rig.om, 1.0);
    rig.om.setFrameTime(0.25);
    rig.om.retireSceneFades(rig.es.scenes);
    const SceneTake back = rig.es.takeScene(1, rig.om, 1.0);  // wide is fading out: not shown
    ASSERT_EQ(back.kind, SceneTake::Kind::Route);
    for (OutputWindow* o : {rig.left, rig.right}) {
        const SceneFade* fade = rig.om.sceneFade(*o);
        ASSERT_NE(fade, nullptr);
        EXPECT_EQ(fade->fromSceneId, rig.next);
        EXPECT_EQ(fade->toSceneId, rig.wide);
        EXPECT_NEAR(sceneFadeMix(*fade, 0.25), 0.75, 1e-6) << "wide keeps its 0.75";
    }

    // A take of the side projector's scene only activates it: the fade carries on.
    EXPECT_EQ(rig.es.takeScene(2, rig.om, 1.0).kind, SceneTake::Kind::Activate);
    EXPECT_NE(rig.om.sceneFade(*rig.left), nullptr);
}

TEST(SceneTakeFadeTest, DeletingASceneMidFadeIsSafe) {
    {
        BlendedRig rig;  // the scene fading out goes
        rig.om.setFrameTime(0.0);
        rig.es.takeScene(3, rig.om, 1.0);
        rig.es.setActiveScene(rig.other);
        ASSERT_TRUE(rig.es.deleteScene(rig.wide, &rig.om));
        EXPECT_EQ(rig.om.sceneFade(*rig.left), nullptr);
        EXPECT_EQ(rig.left->getSceneId(), rig.next) << "the scene fading in shows in full";
        rig.om.setFrameTime(0.5);
        EXPECT_FALSE(rig.om.retireSceneFades(rig.es.scenes));
    }
    {
        BlendedRig rig;  // the scene fading in goes
        rig.om.setFrameTime(0.0);
        rig.es.takeScene(3, rig.om, 1.0);
        ASSERT_TRUE(rig.es.deleteScene(rig.next, &rig.om));
        EXPECT_TRUE(rig.om.sceneFades().empty());
        EXPECT_TRUE(rig.left->getSceneId().isNull()) << "as deleting a shown scene always did";
    }
    {
        BlendedRig rig;  // a scene list replaced without deleteScene (New/Open): retired
        rig.om.setFrameTime(0.0);
        rig.es.takeScene(3, rig.om, 1.0);
        std::vector<std::unique_ptr<Scene>> none;
        rig.om.setFrameTime(0.1);
        EXPECT_TRUE(rig.om.retireSceneFades(none));
        EXPECT_TRUE(rig.om.sceneFades().empty());
    }
}

// Re-routing a fading output by hand (Inspector, Scenes panel) ends its fade.
TEST(SceneTakeFadeTest, ManualRoutingEndsTheFade) {
    BlendedRig rig;
    rig.om.setFrameTime(0.0);
    rig.es.takeScene(3, rig.om, 1.0);
    rig.om.routeScene(*rig.left, rig.es.scenes[1].get(), rig.es.scenes);
    EXPECT_EQ(rig.om.sceneFade(*rig.left), nullptr);
    EXPECT_NE(rig.om.sceneFade(*rig.right), nullptr);
    rig.om.setFrameTime(0.1);
    EXPECT_TRUE(rig.om.retireSceneFades(rig.es.scenes));
    EXPECT_EQ(rig.om.sceneFades().find(rig.left->getId()), nullptr);
}

// Blackout hides the fade but does not stop its clock: lifted after the fade time, the
// new scene shows in full; lifted midway, the fade is where the wall clock says.
TEST(SceneTakeFadeTest, BlackoutDoesNotStopTheFadeClock) {
    BlendedRig rig;
    rig.om.setFrameTime(0.0);
    rig.es.takeScene(3, rig.om, 2.0);
    rig.om.setBlackout(true);
    rig.om.setFrameTime(1.0);
    rig.om.retireSceneFades(rig.es.scenes);
    rig.om.setBlackout(false);
    EXPECT_FLOAT_EQ(sceneFadeMix(*rig.om.sceneFade(*rig.left), 1.0), 0.5f);
    rig.om.setBlackout(true);
    rig.om.setFrameTime(2.5);
    EXPECT_TRUE(rig.om.retireSceneFades(rig.es.scenes));
    rig.om.setBlackout(false);
    EXPECT_EQ(rig.om.sceneFade(*rig.left), nullptr);
}

// Deleting a fading output drops its fade.
TEST(SceneTakeFadeTest, DeletingAnOutputDropsItsFade) {
    BlendedRig rig;
    rig.om.setFrameTime(0.0);
    rig.es.takeScene(3, rig.om, 1.0);
    const UUID leftId = rig.left->getId();
    rig.om.deleteOutput(leftId);
    EXPECT_EQ(rig.om.sceneFades().find(leftId), nullptr);
    EXPECT_NE(rig.om.sceneFade(*rig.right), nullptr);
}

namespace {

// Two quads side by side in the active scene, the left one selected, with the
// Properties panel on an output.
struct PickRig {
    EditorState es;
    Quad* left = nullptr;
    Quad* right = nullptr;

    PickRig() {
        QuadManager& quads = es.quadManager();
        left = quads.createQuad("Left");
        left->getGeometry().setCorners({Vec2(0.0f, 0.0f), Vec2(0.4f, 0.0f), Vec2(0.4f, 1.0f), Vec2(0.0f, 1.0f)});
        right = quads.createQuad("Right");
        right->getGeometry().setCorners({Vec2(0.6f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.6f, 1.0f)});
        quads.selectQuad(left->getId());
        es.currentSelectionType = EditorState::SelectionType::Output;
    }
};

}  // namespace

// Clicking the quad that is already selected, on the canvas, brings Quad Properties
// back (as the Quad List does) and keeps the selection; nothing is reported, so the
// caller keeps its vertex selection.
TEST(QuadSelectionTest, ClickingTheSelectedQuadOnTheCanvasShowsQuadProperties) {
    PickRig rig;
    EXPECT_FALSE(pickQuadOnCanvas(rig.es, rig.es.quadManager(), Vec2(0.2f, 0.5f), false));
    EXPECT_EQ(rig.es.currentSelectionType, EditorState::SelectionType::Quad);
    EXPECT_EQ(rig.es.quadManager().getPrimarySelectedQuad(), rig.left);
}

// A press on the selected quad's handle does the same without picking; another quad
// is picked (reported, so the vertex selection drops); empty canvas changes nothing.
TEST(QuadSelectionTest, HandlesOtherQuadsAndEmptyCanvas) {
    PickRig rig;
    EXPECT_FALSE(pickQuadOnCanvas(rig.es, rig.es.quadManager(), Vec2(0.5f, 0.5f), false));
    EXPECT_EQ(rig.es.currentSelectionType, EditorState::SelectionType::Output) << "empty canvas";

    EXPECT_FALSE(pickQuadOnCanvas(rig.es, rig.es.quadManager(), Vec2(0.7f, 0.5f), true));
    EXPECT_EQ(rig.es.currentSelectionType, EditorState::SelectionType::Quad) << "on the handle";
    EXPECT_EQ(rig.es.quadManager().getPrimarySelectedQuad(), rig.left) << "a handle press never picks";

    rig.es.currentSelectionType = EditorState::SelectionType::Output;
    EXPECT_TRUE(pickQuadOnCanvas(rig.es, rig.es.quadManager(), Vec2(0.7f, 0.5f), false));
    EXPECT_EQ(rig.es.currentSelectionType, EditorState::SelectionType::Quad);
    EXPECT_EQ(rig.es.quadManager().getPrimarySelectedQuad(), rig.right);
}

// The texture Path field names the shown quad's current media: after a video replaces
// an image on the quad it shows the video's path, it follows the selection, and a
// path being typed is kept while nothing changes.
TEST(QuadSelectionTest, ThePathFieldFollowsTheQuadsCurrentMedia) {
    PickRig rig;
    UUID shownQuad;
    std::string shownPath;
    rig.es.quadTexturePaths[rig.left->getId()] = "show/still.png";
    followTexturePath(rig.es, rig.left, shownQuad, shownPath);
    EXPECT_STREQ(rig.es.currentTexturePath, "show/still.png");

    rig.es.quadTexturePaths[rig.left->getId()] = "show/clip.mp4";  // a video assigned
    followTexturePath(rig.es, rig.left, shownQuad, shownPath);
    EXPECT_STREQ(rig.es.currentTexturePath, "show/clip.mp4");

    std::snprintf(rig.es.currentTexturePath, sizeof(rig.es.currentTexturePath), "show/ty");
    followTexturePath(rig.es, rig.left, shownQuad, shownPath);
    EXPECT_STREQ(rig.es.currentTexturePath, "show/ty") << "typing is kept";

    followTexturePath(rig.es, rig.right, shownQuad, shownPath);
    EXPECT_STREQ(rig.es.currentTexturePath, "") << "a quad without media";
    followTexturePath(rig.es, rig.left, shownQuad, shownPath);
    EXPECT_STREQ(rig.es.currentTexturePath, "show/clip.mp4");
}
