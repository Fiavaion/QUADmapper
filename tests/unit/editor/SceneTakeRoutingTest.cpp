// Scene takes on the live routing (ADR-047 §5): EditorState::takeScene against real
// outputs, with and without crossfades. Also slice editing on the same rig (ADR-047
// part B, editor/SliceEditing.hpp): the canvas keys, auto-blend and the canvas size,
// through real commands and undo. And what selecting a quad shows in the Properties
// panel (editor/QuadSelection.hpp). None of that needs GL; the calibration view tests at the
// end drive ViewportEditor in a hidden GL window and skip without one.
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/MeshEditKeys.hpp"
#include "editor/ProjectReplace.hpp"
#include "editor/QuadSelection.hpp"
#include "editor/SliceEditing.hpp"
#include "editor/UndoRouting.hpp"
#include "renderer/Framebuffer.hpp"
#include "renderer/Renderer.hpp"
#include "scene/MeshObject.hpp"
#include "ui/panels/ViewportEditor.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

// Calibrate `output` from the box's eight corners at the pixels where the true
// projector shows them (CalibrationViewEditorTest proves a pick on the calibration view
// lands on the pixel it shows, whatever the slicing). Leaves calibration running.
void pickBoxCorners(EditorState& es, OutputWindow& output, const ProjectorCamera& truth) {
    output.getCalibrationPoints().clear();
    output.getProjector() = ProjectorCamera{};
    output.getProjector().intrinsics = ProjectorIntrinsics::fromThrowRatio(
        1.5f, truth.intrinsics.width, truth.intrinsics.height);
    es.beginCalibration(output);
    for (int i = 0; i < 8; ++i) {
        CalibrationPoint point;
        point.world = Vec3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
        ASSERT_TRUE(truth.project(point.world, point.pixel));
        output.getCalibrationPoints().push_back(point);
    }
    es.calibration.markChanged();
    ASSERT_TRUE(es.calibration.update(output.getCalibrationPoints(), output.getProjector()))
        << es.calibration.status;
}

}  // namespace

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

    clearProjectContent(rig.es, nullptr, &rig.om, nullptr);
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

// Showing Quad Properties (the Quad List or a canvas pick) clears the media bin and
// Outputs tab selections, so the Inspector cannot fall back to either; all on
// EditorState, with no panel.
TEST(QuadSelectionTest, ShowQuadPropertiesClearsMediaAndOutputSelection) {
    PickRig rig;
    rig.es.selectedMediaId = UUID::generate();
    rig.es.selectedOutputId = UUID::generate();
    showQuadProperties(rig.es);
    EXPECT_EQ(rig.es.currentSelectionType, EditorState::SelectionType::Quad);
    EXPECT_TRUE(rig.es.selectedMediaId.isNull());
    EXPECT_TRUE(rig.es.selectedOutputId.isNull());

    rig.es.selectedMediaId = UUID::generate();
    rig.es.selectedOutputId = UUID::generate();
    rig.es.currentSelectionType = EditorState::SelectionType::Media;
    EXPECT_TRUE(pickQuadOnCanvas(rig.es, rig.es.quadManager(), Vec2(0.7f, 0.5f), false));
    EXPECT_TRUE(rig.es.selectedMediaId.isNull()) << "a canvas pick clears them too";
    EXPECT_TRUE(rig.es.selectedOutputId.isNull());

    const UUID media = UUID::generate();
    rig.es.selectedMediaId = media;
    EXPECT_FALSE(pickQuadOnCanvas(rig.es, rig.es.quadManager(), Vec2(0.5f, 0.5f), false));
    EXPECT_EQ(rig.es.selectedMediaId, media) << "empty canvas keeps the selection";
}

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

// ---------------------------------------------------------------------------
// The calibration view on the editor canvas (TD-29, ADR-047 Amendment 6): the real
// ViewportEditor::render, in a hidden GL 3.3 window with an ImGui frame, while an output
// is calibrated. The canvas must show that output's frame as the projector gets it
// (OutputManager::outputFrame, un-turned), wherever the view's zoom and pan put it, and
// a press on it must land on the projector pixel it shows. Skips without GL.
// ---------------------------------------------------------------------------

namespace {

// Hidden editor window with a GL 3.3 core context and an ImGui context, as Application
// has when it draws the canvas, RAII; ok == false means no GL.
struct EditorWindow {
    GLFWwindow* win = nullptr;
    ImGuiContext* imgui = nullptr;
    bool ok = false;
    EditorWindow() {
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        win = glfwCreateWindow(400, 300, "calibration-view-editor", nullptr, nullptr);
        glfwDefaultWindowHints();
        if (!win) { glfwTerminate(); return; }
        glfwMakeContextCurrent(win);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            glfwDestroyWindow(win); win = nullptr; glfwTerminate(); return;
        }
        imgui = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(400.0f, 300.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        ok = true;
    }
    ~EditorWindow() {
        if (imgui) ImGui::DestroyContext(imgui);
        if (win) { glfwDestroyWindow(win); glfwTerminate(); }
    }
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;
};

// A texture with detail every few texels, sampled nearest (so a misplaced view shows).
std::unique_ptr<Texture> checkerTexture(int size, int seed) {
    std::vector<unsigned char> px(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            unsigned char* p = &px[(static_cast<size_t>(y) * size + x) * 4];
            p[0] = static_cast<unsigned char>(((x / 4) * 60 + seed * 40) & 255);
            p[1] = static_cast<unsigned char>(((y / 4) * 70 + seed * 90) & 255);
            p[2] = static_cast<unsigned char>((((x / 4) ^ (y / 4)) * 50 + seed) & 255);
            p[3] = 255;
        }
    }
    auto texture = std::make_unique<Texture>();
    texture->create(size, size, 4, px.data());
    texture->setFilterMode(false);
    return texture;
}

std::vector<unsigned char> readBoundPixels(int w, int h) {
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return pixels;
}

// One calibrated output: its turn, the scene's canvas, its one slice (input rectangle,
// warp, left blend width), the view's zoom and pan, and its window size (the
// resolution stays 320x180).
struct ViewCase {
    const char* name;
    Rotation rotation;
    IVec2 canvas;
    CanvasRect input;
    std::array<Vec2, 4> warp;
    float blendLeft;
    float zoom;
    Vec2 pan;
    IVec2 windowSize;
};

const std::array<Vec2, 4> KEYSTONE = {Vec2(0.08f, 0.04f), Vec2(0.97f, 0.0f), Vec2(0.88f, 1.0f),
                                      Vec2(0.0f, 0.93f)};
const std::array<Vec2, 4> UNWARPED = {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f),
                                      Vec2(0.0f, 1.0f)};

std::vector<ViewCase> viewCases() {
    return {
        {"R0 default slicing", Rotation::R0, IVec2(320, 180), CanvasRect{0, 0, 320, 180},
         UNWARPED, 0.0f, 1.0f, Vec2(0.0f), IVec2(320, 180)},
        {"R0 offset keystoned slice of a two-frame canvas, zoom 1.4 and pan", Rotation::R0,
         IVec2(640, 180), CanvasRect{200, 18, 320, 144}, KEYSTONE, 0.0f, 1.4f,
         Vec2(0.06f, -0.04f), IVec2(320, 180)},
        {"R90 blended keystoned slice, zoom 0.8 and pan", Rotation::R90, IVec2(300, 320),
         CanvasRect{60, 0, 216, 320}, KEYSTONE, 60.0f, 0.8f, Vec2(-0.03f, 0.02f),
         IVec2(320, 180)},
        {"R270 keystoned, window 256x200 unlike the resolution", Rotation::R270,
         IVec2(180, 320), CanvasRect{0, 0, 180, 320}, KEYSTONE, 0.0f, 1.2f,
         Vec2(0.02f, 0.03f), IVec2(256, 200)},
        {"R180 offset blended slice", Rotation::R180, IVec2(640, 180),
         CanvasRect{300, 0, 320, 180}, KEYSTONE, 40.0f, 1.0f, Vec2(0.0f), IVec2(320, 180)},
    };
}

// The editor with one output calibrated: two textured quads and a cube in the active
// scene, the output showing it through the case's slice, its projector fitted to the
// cube, and the output frames drawn once (the view draws from their canvas).
struct CalibrationViewRig {
    GLFWwindow* win;
    EditorState es;
    ViewportEditor viewport;  // owned by EditorPanels in the app (ADR-051 E3)
    OutputManager om;
    OutputWindow* output = nullptr;
    Renderer renderer;
    std::unique_ptr<Texture> mediaA, mediaB;
    std::unordered_map<UUID, Texture*> textures;
    Framebuffer editor{Framebuffer::Depth::With};
    int fbWidth = 0, fbHeight = 0, winWidth = 0, winHeight = 0;
    bool ok = false;

    CalibrationViewRig(GLFWwindow* window, const ViewCase& c) : win(window) {
        if (!om.initialize(win).isSuccess()) return;
        es.outputResolver.setOutputManager(&om);
        output = om.createOutput("Calibrated");
        if (!output) return;
        output->setResolution(IVec2(320, 180));
        output->setRotation(c.rotation);
        glfwSetWindowSize(output->getGLFWwindow(), c.windowSize.x, c.windowSize.y);
        glfwMakeContextCurrent(win);

        Scene& scene = es.activeScene();
        scene.setCanvasSize(c.canvas);
        mediaA = checkerTexture(32, 1);
        mediaB = checkerTexture(32, 2);
        Quad* a = scene.getQuadManager().createQuad("A");
        a->getGeometry().setCorners({Vec2(0.30f, 0.10f), Vec2(0.70f, 0.15f), Vec2(0.65f, 0.60f),
                                     Vec2(0.28f, 0.55f)});
        Quad* b = scene.getQuadManager().createQuad("B");
        b->getGeometry().setCorners({Vec2(0.45f, 0.50f), Vec2(0.95f, 0.45f), Vec2(0.90f, 0.95f),
                                     Vec2(0.50f, 0.92f)});
        textures[a->getId()] = mediaA.get();
        textures[b->getId()] = mediaB.get();
        MeshObject* cube = scene.createObject("Cube");
        MeshData& data = cube->editMesh();
        const float h = 0.15f;
        for (int k = 0; k < 8; ++k) {
            data.positions.push_back(Vec3((k & 1) ? h : -h, (k & 2) ? h : -h, (k & 4) ? h : -h));
        }
        data.indices = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                        2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
        cube->setTransform(glm::rotate(glm::mat4(1.0f), 0.6f, Vec3(0.3f, 1.0f, 0.2f)));

        OutputManager::showScene(*output, &scene, es.scenes);
        Slice slice = makeFullCanvasSlice(c.canvas);
        slice.input = c.input;
        slice.warp.setCorners({c.warp[0], c.warp[1], c.warp[2], c.warp[3]});
        slice.blend[SLICE_EDGE_LEFT].width = c.blendLeft;
        output->getSlices() = {slice};
        const IVec2 frame = output->getEffectiveResolution();
        Vec3 lo, hi;
        if (!scene.visibleObjectBounds(lo, hi)) return;
        output->getProjector() =
            ProjectorCamera::fitToOutput(ProjectorCamera{}, frame.x, frame.y, &lo, &hi);

        if (!renderer.initialize()) return;
        om.renderOutputFrames(es.scenes, textures, renderer);
        es.beginCalibration(*output);

        glfwGetFramebufferSize(win, &fbWidth, &fbHeight);
        glfwGetWindowSize(win, &winWidth, &winHeight);
        if (!editor.resize(fbWidth, fbHeight)) return;
        es.canvasRegionPos = Vec2(20.0f, 30.0f);
        es.canvasRegionSize = Vec2(static_cast<float>(winWidth) - 40.0f,
                                   static_cast<float>(winHeight) - 50.0f);
        es.canvasZoom = c.zoom;
        es.canvasPan = c.pan;
        ok = true;
    }
    ~CalibrationViewRig() {
        glfwMakeContextCurrent(win);
        renderer.shutdown();
        mediaA.reset();
        mediaB.reset();
        om.shutdown();
    }
    CalibrationViewRig(const CalibrationViewRig&) = delete;
    CalibrationViewRig& operator=(const CalibrationViewRig&) = delete;

    // The projector's own frame, as it is sent to the output window (turned).
    std::vector<unsigned char> projectorFrame(int& width, int& height) const {
        glfwGetFramebufferSize(output->getGLFWwindow(), &width, &height);
        om.outputFrame(output->getId())->bind();
        std::vector<unsigned char> px = readBoundPixels(width, height);
        Framebuffer::unbind();
        return px;
    }

    // One editor frame over the editor's grey background, with the mouse at `pointer`;
    // returns the editor framebuffer.
    std::vector<unsigned char> frame(const ViewportEditor::Pointer& pointer) {
        editor.bind();
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.2f, 0.3f, 0.4f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        ImGui::NewFrame();
        EditorContext ctx{es, nullptr, nullptr, nullptr, &om, &renderer, win};
        viewport.render(ctx, pointer);
        ImGui::EndFrame();
        std::vector<unsigned char> px = readBoundPixels(fbWidth, fbHeight);
        Framebuffer::unbind();
        return px;
    }

    // Where the view shows the point `f` of the output frame ([0,1], +y down), in window
    // coordinates: the view transform of the editor's last frame.
    Vec2 windowPoint(const Vec2& f) const {
        return es.canvasRegionPos +
               (es.lastViewOffset + es.lastViewScale * f) * es.canvasRegionSize;
    }
};

// The frame point `f` (+y down, un-turned) -> that point of the turned frame.
Vec2 turned(Rotation rotation, const Vec2& f) {
    switch (rotation) {
    case Rotation::R90: return Vec2(1.0f - f.y, f.x);
    case Rotation::R180: return Vec2(1.0f - f.x, 1.0f - f.y);
    case Rotation::R270: return Vec2(f.y, 1.0f - f.x);
    default: return f;
    }
}

}  // namespace

// The canvas shows the calibrated output's frame, whatever its slicing, turn and window
// size and the view's zoom and pan: the frame keeps its aspect on the canvas; every
// editor pixel inside it (through the view's scale and offset) matches the projector
// frame's pixel there, away from edges in the picture; the frame shows the quads and
// the cube; and the canvas region around it keeps the editor's background.
TEST(CalibrationViewEditorTest, TheCanvasShowsTheOutputFrameWhateverTheSlicing) {
    EditorWindow gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    for (const ViewCase& c : viewCases()) {
        SCOPED_TRACE(c.name);
        CalibrationViewRig rig(gl.win, c);
        ASSERT_TRUE(rig.ok);
        ASSERT_EQ(calibrationViewOutput(rig.es, &rig.om), rig.output);
        int fw = 0, fh = 0;
        const std::vector<unsigned char> truth = rig.projectorFrame(fw, fh);
        const std::vector<unsigned char> view = rig.frame(ViewportEditor::Pointer{});

        const int W = rig.fbWidth, H = rig.fbHeight;
        const float dpiX = static_cast<float>(W) / static_cast<float>(rig.winWidth);
        const float dpiY = static_cast<float>(H) / static_cast<float>(rig.winHeight);
        const Vec2 scale = rig.es.lastViewScale, offset = rig.es.lastViewOffset;
        const Vec2 regionPos = rig.es.canvasRegionPos, regionSize = rig.es.canvasRegionSize;
        const IVec2 frameSize = rig.output->getEffectiveResolution();
        EXPECT_NEAR((scale.x * regionSize.x) / (scale.y * regionSize.y),
                    static_cast<float>(frameSize.x) / static_cast<float>(frameSize.y), 1e-3f)
            << "the frame keeps its shape on the canvas";
        int compared = 0, lit = 0, mismatched = 0, around = 0, aroundBlack = 0;
        for (int gy = 0; gy < H; ++gy) {
            for (int gx = 0; gx < W; ++gx) {
                // The pixel's centre in window coordinates, then on the frame.
                const float lx = (static_cast<float>(gx) + 0.5f) / dpiX;
                const float ly = static_cast<float>(rig.winHeight) - (static_cast<float>(gy) + 0.5f) / dpiY;
                const Vec2 vn((lx - regionPos.x) / regionSize.x, (ly - regionPos.y) / regionSize.y);
                if (vn.x < 0.0f || vn.x > 1.0f || vn.y < 0.0f || vn.y > 1.0f) continue;
                const Vec2 f = (vn - offset) / scale;
                const unsigned char* a = &view[(static_cast<size_t>(gy) * W + gx) * 4];
                if (f.x < -0.02f || f.x > 1.02f || f.y < -0.02f || f.y > 1.02f) {
                    ++around;
                    if (a[0] < 8 && a[1] < 8 && a[2] < 8) ++aroundBlack;
                    continue;
                }
                if (f.x < 0.01f || f.x > 0.99f || f.y < 0.01f || f.y > 0.99f) continue;
                const Vec2 t = turned(c.rotation, f);
                const int px = std::min(static_cast<int>(t.x * static_cast<float>(fw)), fw - 1);
                const int py = std::min(static_cast<int>((1.0f - t.y) * static_cast<float>(fh)), fh - 1);
                const unsigned char* b = &truth[(static_cast<size_t>(py) * fw + px) * 4];
                // Skip edges in the projector frame, where the two samplings differ.
                bool edge = false;
                for (int dy = -2; dy <= 2 && !edge; ++dy) {
                    for (int dx = -2; dx <= 2 && !edge; ++dx) {
                        const int qx = std::clamp(px + dx, 0, fw - 1);
                        const int qy = std::clamp(py + dy, 0, fh - 1);
                        const unsigned char* q = &truth[(static_cast<size_t>(qy) * fw + qx) * 4];
                        edge = std::abs(q[0] - b[0]) + std::abs(q[1] - b[1]) + std::abs(q[2] - b[2]) > 30;
                    }
                }
                if (edge) continue;
                ++compared;
                if (b[0] + b[1] + b[2] > 30) ++lit;
                const int d = std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
                if (d > 24) ++mismatched;
            }
        }
        EXPECT_GT(compared, W * H / 10) << "the frame must fill much of the canvas";
        EXPECT_GT(lit, compared / 10) << "the frame must show the quads and the cube";
        EXPECT_LE(mismatched, compared / 100) << "of " << compared << " pixels compared";
        EXPECT_EQ(aroundBlack, 0) << "of " << around << " pixels around the frame";
    }
}

// A press on the calibration view lands on the projector pixel the view shows there:
// Add Point's second click places the point at that pixel, and a drag moves it by the
// projector pixels the mouse moved over, whatever the slicing, turn, zoom and pan.
TEST(CalibrationViewEditorTest, PicksOnTheCanvasLandOnTheProjectorPixelsShown) {
    EditorWindow gl;
    if (!gl.ok) GTEST_SKIP() << "No GL 3.3 context available";

    for (const ViewCase& c : viewCases()) {
        SCOPED_TRACE(c.name);
        CalibrationViewRig rig(gl.win, c);
        ASSERT_TRUE(rig.ok);
        rig.frame(ViewportEditor::Pointer{});  // the view transform for this output
        const ProjectorIntrinsics& k = rig.output->getProjector().intrinsics;
        const Vec2 size(static_cast<float>(k.width), static_cast<float>(k.height));
        auto pointerAt = [&](const Vec2& pixel, bool down) {
            return ViewportEditor::Pointer{rig.windowPoint(pixel / size), down};
        };
        const Vec2 target(0.42f * size.x + 0.3f, 0.57f * size.y + 0.6f);

        CalibrationSession& cal = rig.es.calibration;
        cal.pick = CalibrationSession::Pick::Pixel;
        cal.pendingWorld = Vec3(0.1f, 0.2f, 0.3f);
        rig.frame(pointerAt(target, true));
        rig.frame(pointerAt(target, false));
        const auto& points = rig.output->getCalibrationPoints();
        ASSERT_EQ(points.size(), 1u);
        EXPECT_EQ(points[0].world, Vec3(0.1f, 0.2f, 0.3f));
        EXPECT_NEAR(points[0].pixel.x, target.x, 0.01f);
        EXPECT_NEAR(points[0].pixel.y, target.y, 0.01f);

        const Vec2 moved = target + Vec2(12.0f, -7.0f);
        rig.frame(pointerAt(target, true));
        rig.frame(pointerAt(moved, true));
        rig.frame(pointerAt(moved, false));
        EXPECT_NEAR(points[0].pixel.x, moved.x, 0.01f);
        EXPECT_NEAR(points[0].pixel.y, moved.y, 0.01f);
    }
}
