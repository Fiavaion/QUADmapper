#include <gtest/gtest.h>
#include "scene/CalibrationSession.hpp"
#include "scene/Commands.hpp"
#include "core/CommandHistory.hpp"

#include <map>
#include <memory>
#include <vector>

namespace quadmapper {
namespace test {

namespace {

constexpr int WIDTH = 1920;
constexpr int HEIGHT = 1080;

ProjectorIntrinsics throwLens() { return ProjectorIntrinsics::fromThrowRatio(1.5f, WIDTH, HEIGHT); }

// The true projector: looking at a 2x2x2 box from an oblique angle.
ProjectorCamera trueCamera() {
    return orbitCamera(Vec3(0.0f), 25.0f, 15.0f, 6.0f, WIDTH, HEIGHT);
}

// Exact pairs for the given world points as seen by `camera`.
std::vector<CalibrationPoint> pairsFor(const ProjectorCamera& camera, const std::vector<Vec3>& world) {
    std::vector<CalibrationPoint> points;
    for (const Vec3& w : world) {
        CalibrationPoint p;
        p.world = w;
        EXPECT_TRUE(camera.project(w, p.pixel));
        points.push_back(p);
    }
    return points;
}

std::vector<Vec3> cubeCorners() {
    std::vector<Vec3> corners;
    for (int i = 0; i < 8; ++i)
        corners.emplace_back((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
    return corners;
}

bool sameCamera(const ProjectorCamera& a, const ProjectorCamera& b) {
    return a.calibrated == b.calibrated && a.pose.rotation == b.pose.rotation &&
           a.pose.translation == b.pose.translation && a.intrinsics.fx == b.intrinsics.fx &&
           a.intrinsics.cx == b.intrinsics.cx;
}

}  // namespace

// ---- Canvas <-> pixel mapping ----------------------------------------------

TEST(CalibrationCanvasTest, ViewTransformRoundTripsWithZoomAndPan) {
    // As ViewportEditor builds it: fit 0.92 x 0.6, zoom 2.5, pan (0.1, -0.05).
    const float zoom = 2.5f;
    CanvasView view;
    view.scale = Vec2(0.92f * zoom, 0.6f * zoom);
    view.offset = (Vec2(1.0f) - view.scale) * 0.5f + Vec2(0.1f, -0.05f);

    const Vec2 canvas(0.3f, 0.8f);
    const Vec2 region = view.toRegion(canvas);
    EXPECT_NEAR(view.toCanvas(region).x, canvas.x, 1e-6f);
    EXPECT_NEAR(view.toCanvas(region).y, canvas.y, 1e-6f);
    // With the pan the canvas centre sits off the region centre by exactly the pan.
    EXPECT_NEAR(view.toRegion(Vec2(0.5f)).x, 0.6f, 1e-6f);
    EXPECT_NEAR(view.toRegion(Vec2(0.5f)).y, 0.45f, 1e-6f);
}

TEST(CalibrationCanvasTest, CanvasMapsToTheWholeProjectorImage) {
    const ProjectorIntrinsics k = throwLens();
    EXPECT_EQ(canvasToPixel(Vec2(0.0f), k), Vec2(0.0f));
    EXPECT_EQ(canvasToPixel(Vec2(1.0f), k), Vec2(WIDTH, HEIGHT));
    EXPECT_EQ(canvasToPixel(Vec2(0.25f, 0.5f), k), Vec2(480.0f, 540.0f));
    const Vec2 pixel(1234.5f, 77.25f);
    EXPECT_NEAR(canvasToPixel(pixelToCanvas(pixel, k), k).x, pixel.x, 1e-3f);
    EXPECT_NEAR(canvasToPixel(pixelToCanvas(pixel, k), k).y, pixel.y, 1e-3f);
}

TEST(CalibrationCanvasTest, MouseToPixelUnderZoomAndPan) {
    // Zoomed 2x around the centre: the region's centre is still the image centre, and a
    // quarter of the region right of it is an eighth of the image.
    CanvasView view;
    view.scale = Vec2(2.0f);
    view.offset = Vec2(-0.5f);
    const ProjectorIntrinsics k = throwLens();
    const Vec2 centre = canvasToPixel(view.toCanvas(Vec2(0.5f)), k);
    EXPECT_NEAR(centre.x, WIDTH * 0.5f, 1e-3f);
    EXPECT_NEAR(centre.y, HEIGHT * 0.5f, 1e-3f);
    const Vec2 right = canvasToPixel(view.toCanvas(Vec2(0.75f, 0.5f)), k);
    EXPECT_NEAR(right.x, WIDTH * 0.625f, 1e-3f);
}

// ---- Orbit camera ------------------------------------------------------------

TEST(CalibrationOrbitTest, LooksAtTheTargetFromTheDistance) {
    const Vec3 target(1.0f, 2.0f, -3.0f);
    const ProjectorCamera camera = orbitCamera(target, 40.0f, 30.0f, 7.0f, 800, 600);
    EXPECT_NEAR(glm::length(camera.position() - target), 7.0f, 1e-4f);
    Vec2 pixel;
    ASSERT_TRUE(camera.project(target, pixel));
    EXPECT_NEAR(pixel.x, 400.0f, 1e-2f);
    EXPECT_NEAR(pixel.y, 300.0f, 1e-2f);
    // World up is screen up: a point above the target lands higher (smaller v).
    Vec2 above;
    ASSERT_TRUE(camera.project(target + Vec3(0.0f, 0.5f, 0.0f), above));
    EXPECT_LT(above.y, pixel.y);
}

TEST(CalibrationOrbitTest, ZeroYawStandsOnThePlusZSide) {
    const ProjectorCamera camera = orbitCamera(Vec3(0.0f), 0.0f, 0.0f, 5.0f, 800, 600);
    EXPECT_NEAR(camera.position().z, 5.0f, 1e-4f);
    Vec2 right;
    ASSERT_TRUE(camera.project(Vec3(1.0f, 0.0f, 0.0f), right));
    EXPECT_GT(right.x, 400.0f);  // world +X is screen right
}

// ---- Live-solve policy -------------------------------------------------------

TEST(CalibrationSessionTest, RequiredPointsFollowTheMode) {
    EXPECT_EQ(requiredCalibrationPoints(CalibrationMode::Auto), 4);
    EXPECT_EQ(requiredCalibrationPoints(CalibrationMode::PoseOnly), 4);
    EXPECT_EQ(requiredCalibrationPoints(CalibrationMode::Full), 6);
}

TEST(CalibrationSessionTest, TooFewEnabledPointsDoNotSolve) {
    const ProjectorCamera truth = trueCamera();
    auto points = pairsFor(truth, cubeCorners());
    points.resize(5);
    points[3].enabled = false;  // five pairs, three enabled
    points[4].enabled = false;

    ProjectorCamera projector;
    projector.intrinsics = throwLens();
    const ProjectorCamera before = projector;
    CalibrationSession session;
    session.begin(points, projector);
    EXPECT_FALSE(session.update(points, projector));
    EXPECT_TRUE(sameCamera(projector, before));
    EXPECT_FALSE(session.solved);
    EXPECT_FALSE(session.statusIsError);
    EXPECT_EQ(session.pointsUsed, 3);
}

TEST(CalibrationSessionTest, FullModeNeedsSixPoints) {
    const ProjectorCamera truth = trueCamera();
    auto points = pairsFor(truth, cubeCorners());
    points.resize(5);
    ProjectorCamera projector;
    projector.intrinsics = throwLens();
    const ProjectorCamera before = projector;
    CalibrationSession session;
    session.begin(points, projector);
    session.mode = CalibrationMode::Full;
    EXPECT_FALSE(session.update(points, projector));
    EXPECT_TRUE(sameCamera(projector, before));

    session.mode = CalibrationMode::PoseOnly;  // the same five points solve pose-only
    session.markChanged();
    EXPECT_TRUE(session.update(points, projector));
}

TEST(CalibrationSessionTest, EnoughPointsSolveAndCalibrateTheProjector) {
    const ProjectorCamera truth = trueCamera();
    const auto points = pairsFor(truth, cubeCorners());
    ProjectorCamera projector;
    projector.intrinsics = throwLens();
    CalibrationSession session;
    session.begin(points, projector);
    ASSERT_TRUE(session.update(points, projector)) << session.status;
    EXPECT_TRUE(projector.calibrated);
    EXPECT_TRUE(session.solved);
    EXPECT_FALSE(session.status.empty());
    EXPECT_LT(session.rmsError, 0.05f);
    EXPECT_EQ(session.pointsUsed, 8);
    for (const auto& p : points) EXPECT_LT(reprojectionError(p, projector), 0.1f);
    // Auto with 8 non-coplanar points solves the lens, which pose-only solves then keep.
    EXPECT_NEAR(session.lens.fx, truth.intrinsics.fx, 1.0f);
}

TEST(CalibrationSessionTest, SolverWarningReachesTheSession) {
    // GUI QA: the solver's warnings never reached the panel. Five pairs with one
    // mis-clicked: pose-only solves, but too few pairs to single out the wrong one.
    auto points = pairsFor(trueCamera(), cubeCorners());
    points.resize(5);
    points[1].pixel += Vec2(60.0f, -40.0f);
    ProjectorCamera projector;
    projector.intrinsics = trueCamera().intrinsics;  // pose-only: the lens must be right
    CalibrationSession session;
    session.begin(points, projector);
    ASSERT_TRUE(session.update(points, projector)) << session.status;
    EXPECT_TRUE(session.solved);
    EXPECT_FALSE(session.warning.empty());

    // A clean solve clears it.
    points[1] = pairsFor(trueCamera(), cubeCorners())[1];
    session.markChanged();
    ASSERT_TRUE(session.update(points, projector)) << session.status;
    EXPECT_TRUE(session.warning.empty()) << session.warning;
}

TEST(CalibrationSessionTest, SolvesOnlyAfterAChange) {
    const auto points = pairsFor(trueCamera(), cubeCorners());
    ProjectorCamera projector;
    projector.intrinsics = throwLens();
    CalibrationSession session;
    session.begin(points, projector);
    EXPECT_TRUE(session.update(points, projector));
    EXPECT_FALSE(session.isDirty());
    EXPECT_FALSE(session.update(points, projector));  // nothing changed this frame
    session.markChanged();
    EXPECT_TRUE(session.update(points, projector));
}

TEST(CalibrationSessionTest, SolverErrorKeepsThePreviousCamera) {
    const auto good = pairsFor(trueCamera(), cubeCorners());
    ProjectorCamera projector;
    projector.intrinsics = throwLens();
    CalibrationSession session;
    session.begin(good, projector);
    ASSERT_TRUE(session.update(good, projector));
    const ProjectorCamera before = projector;

    // Four points on one line: the solver refuses.
    std::vector<CalibrationPoint> collinear;
    for (int i = 0; i < 4; ++i) {
        CalibrationPoint p;
        p.world = Vec3(0.3f * i, 0.0f, 0.0f);
        p.pixel = Vec2(100.0f + 50.0f * i, 200.0f + 30.0f * i * i);
        collinear.push_back(p);
    }
    auto expected = calibrateProjector(collinear, session.lens, session.mode);
    ASSERT_FALSE(expected);

    session.markChanged();
    EXPECT_FALSE(session.update(collinear, projector));
    EXPECT_TRUE(sameCamera(projector, before));
    EXPECT_FALSE(session.solved);
    EXPECT_TRUE(session.statusIsError);
    EXPECT_EQ(session.status, expected.error().message);  // verbatim
}

TEST(CalibrationSessionTest, ReprojectionErrorMatchesTheSolverPerPointError) {
    auto points = pairsFor(trueCamera(), cubeCorners());
    points[2].pixel += Vec2(4.0f, -3.0f);  // one slightly mis-clicked pair
    auto result = calibrateProjector(points, throwLens(), CalibrationMode::Auto);
    ASSERT_TRUE(result);
    const CalibrationResult& r = result.value();
    ASSERT_EQ(r.perPointError.size(), points.size());
    for (size_t i = 0; i < points.size(); ++i)
        EXPECT_NEAR(reprojectionError(points[i], r.camera), r.perPointError[i], 1e-2f);
}

TEST(CalibrationSessionTest, RemovingAPointKeepsTheSelectionOnItsPoint) {
    std::vector<CalibrationPoint> points(4);
    CalibrationSession session;
    session.selected = 2;
    session.removePoint(points, 0);
    EXPECT_EQ(points.size(), 3u);
    EXPECT_EQ(session.selected, 1);
    EXPECT_TRUE(session.isDirty());
    session.removePoint(points, 1);
    EXPECT_EQ(session.selected, -1);
    session.removePoint(points, 7);  // out of range: ignored
    EXPECT_EQ(points.size(), 2u);
}

// ---- Calibration undo (TD-17) --------------------------------------------------------

namespace {

// Outputs by id, as OutputManager holds them; the session is reachable only for the
// output being calibrated, as EditorState's resolver does it.
class FakeOutputs : public OutputResolver {
public:
    struct Output {
        std::vector<CalibrationPoint> points;
        ProjectorCamera projector;
    };
    std::map<UUID, Output> outputs;
    UUID calibrating;
    CalibrationSession* session = nullptr;

    CalibrationTarget findCalibration(const UUID& id) override {
        auto it = outputs.find(id);
        if (it == outputs.end()) return {};
        return {&it->second.points, &it->second.projector, id == calibrating ? session : nullptr};
    }
};

}  // namespace

class CalibrationUndoTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto pairs = pairsFor(trueCamera(), cubeCorners());
        spare_ = pairs.back();
        pairs.pop_back();  // seven points: enough to solve, one left to add
        outputs_.outputs[id_] = {pairs, ProjectorCamera{}};
        projector().intrinsics = throwLens();
        outputs_.calibrating = id_;
        outputs_.session = &session_;
        session_.begin(points(), projector());
        endFrame();  // the first frame solves the stored points; no step
        ASSERT_TRUE(projector().calibrated) << session_.status;
        ASSERT_FALSE(history_.canUndo());
    }

    std::vector<CalibrationPoint>& points() { return outputs_.outputs[id_].points; }
    ProjectorCamera& projector() { return outputs_.outputs[id_].projector; }

    // One frame's end as CalibrationPanel runs it: the live solve, then the recording.
    void endFrame() {
        session_.update(points(), projector());
        if (auto edit = session_.finishFrame(points(), projector())) {
            history_.execute(std::make_unique<CalibrationEditCommand>(OutputRef{&outputs_, id_},
                                                                      std::move(*edit)));
        }
    }

    void nudge(int index, const Vec2& by) {
        points()[static_cast<size_t>(index)].pixel += by;
        session_.noteEdit("Move calibration point " + std::to_string(index + 1),
                          CalibrationSession::moveKey(index));
        session_.markChanged();
        endFrame();
    }

    FakeOutputs outputs_;
    UUID id_ = UUID::generate();
    CalibrationSession session_;
    CommandHistory history_;
    CalibrationPoint spare_;
};

TEST_F(CalibrationUndoTest, AddPointIsOneStepAndUndoRestoresTheSolvedProjector) {
    const ProjectorCamera solvedWith7 = projector();
    const std::string status7 = session_.status;
    const float rms7 = session_.rmsError;

    points().push_back(spare_);
    session_.noteEdit("Add calibration point 8");
    session_.markChanged();
    endFrame();
    ASSERT_EQ(history_.getUndoCount(), 1u);
    EXPECT_EQ(history_.getUndoDescription(), "Add calibration point 8");
    EXPECT_EQ(session_.pointsUsed, 8);
    const ProjectorCamera solvedWith8 = projector();

    ASSERT_TRUE(history_.undo());
    EXPECT_EQ(points().size(), 7u);
    EXPECT_TRUE(sameCamera(projector(), solvedWith7));
    EXPECT_EQ(session_.status, status7);
    EXPECT_FLOAT_EQ(session_.rmsError, rms7);
    EXPECT_EQ(session_.pointsUsed, 7);
    EXPECT_FALSE(session_.isDirty());  // the snapshot holds the solve: none is run
    EXPECT_FALSE(session_.update(points(), projector()));

    ASSERT_TRUE(history_.redo());
    EXPECT_EQ(points().size(), 8u);
    EXPECT_TRUE(sameCamera(projector(), solvedWith8));
    endFrame();  // restoring is not itself an edit
    EXPECT_EQ(history_.getUndoCount(), 1u);
    EXPECT_FALSE(history_.canRedo());
}

TEST_F(CalibrationUndoTest, MovesOfOnePointMergeLikeCornerMoves) {
    const Vec2 start = points()[2].pixel;
    const ProjectorCamera before = projector();
    nudge(2, Vec2(1.0f, 0.0f));   // arrow
    nudge(2, Vec2(10.0f, 0.0f));  // Shift+arrow
    nudge(2, Vec2(0.0f, 3.5f));   // a drag frame
    EXPECT_EQ(history_.getUndoCount(), 1u);
    nudge(3, Vec2(-1.0f, 0.0f));  // another point: its own step
    EXPECT_EQ(history_.getUndoCount(), 2u);

    history_.undo();
    EXPECT_EQ(points()[2].pixel, start + Vec2(11.0f, 3.5f));
    history_.undo();
    EXPECT_EQ(points()[2].pixel, start);
    EXPECT_TRUE(sameCamera(projector(), before));
    history_.redo();
    EXPECT_EQ(points()[2].pixel, start + Vec2(11.0f, 3.5f));
}

TEST_F(CalibrationUndoTest, EnableDeleteAndClearAreEachUndoable) {
    const auto original = points();

    points()[1].enabled = false;
    session_.noteEdit("Disable calibration point 2");
    session_.markChanged();
    endFrame();
    session_.selected = 4;
    session_.noteEdit("Delete calibration point 5");
    session_.removePoint(points(), 4);
    endFrame();
    points().clear();
    session_.noteEdit("Clear calibration points");
    session_.markChanged();
    endFrame();
    ASSERT_EQ(history_.getUndoCount(), 3u);
    EXPECT_FALSE(session_.solved);  // too few points

    history_.undo();  // clear
    EXPECT_EQ(points().size(), 6u);
    history_.undo();  // delete: back at its index
    ASSERT_EQ(points().size(), 7u);
    EXPECT_EQ(points()[4].pixel, original[4].pixel);
    EXPECT_FALSE(points()[1].enabled);
    history_.undo();  // disable
    EXPECT_TRUE(points()[1].enabled);
    EXPECT_TRUE(session_.solved);
}

TEST_F(CalibrationUndoTest, LensEditsMergeAndUndoRestoresTheLensAndCamera) {
    const ProjectorIntrinsics lens = session_.lens;
    const ProjectorCamera before = projector();
    session_.mode = CalibrationMode::PoseOnly;  // a lens change then re-solves the pose
    session_.noteEdit("Change calibration lens mode");
    session_.markChanged();
    endFrame();
    for (float ratio : {1.2f, 1.25f, 1.3f}) {  // typing 1.3 into the field
        session_.lens = ProjectorIntrinsics::fromThrowRatio(ratio, WIDTH, HEIGHT);
        session_.noteEdit("Change projector lens", -1);
        session_.markChanged();
        endFrame();
    }
    EXPECT_EQ(history_.getUndoCount(), 2u);  // the mode, and one lens step
    EXPECT_NEAR(session_.lens.fx, 1.3f * WIDTH, 1e-3f);

    history_.undo();
    EXPECT_EQ(session_.mode, CalibrationMode::PoseOnly);
    history_.undo();
    EXPECT_EQ(session_.mode, CalibrationMode::Auto);
    EXPECT_FLOAT_EQ(session_.lens.fx, lens.fx);
    EXPECT_TRUE(sameCamera(projector(), before));
}

TEST_F(CalibrationUndoTest, ResetViewUndoesAndRedoesWithoutASolve) {
    const ProjectorCamera calibrated = projector();
    // As the panel's Reset view does it: no markChanged, or the points would re-solve.
    projector() = ProjectorCamera::framing(Vec3(-1.0f), Vec3(1.0f), session_.lens);
    session_.solved = false;
    session_.status = "View reset: edit a point to solve again";
    session_.noteEdit("Reset projector view");
    endFrame();
    ASSERT_EQ(history_.getUndoCount(), 1u);
    const ProjectorCamera reset = projector();
    EXPECT_FALSE(reset.calibrated);

    history_.undo();
    EXPECT_TRUE(sameCamera(projector(), calibrated));
    EXPECT_TRUE(session_.solved);
    history_.redo();
    EXPECT_TRUE(sameCamera(projector(), reset));
    EXPECT_FALSE(session_.update(points(), projector()));  // stays reset
    EXPECT_FALSE(projector().calibrated);
    EXPECT_EQ(session_.status, "View reset: edit a point to solve again");
}

TEST_F(CalibrationUndoTest, AfterLeavingCalibrationUndoStillRestoresTheOutput) {
    const Vec2 start = points()[0].pixel;
    const ProjectorCamera before = projector();
    nudge(0, Vec2(25.0f, 0.0f));
    outputs_.calibrating = UUID();  // calibration ended: no session for this output
    const ProjectorIntrinsics otherLens = ProjectorIntrinsics::fromThrowRatio(0.8f, WIDTH, HEIGHT);
    session_.lens = otherLens;  // the session now belongs to nothing (or another output)

    ASSERT_TRUE(history_.undo());
    EXPECT_EQ(points()[0].pixel, start);
    EXPECT_TRUE(sameCamera(projector(), before));
    EXPECT_FLOAT_EQ(session_.lens.fx, otherLens.fx);  // untouched
    ASSERT_TRUE(history_.redo());
    EXPECT_EQ(points()[0].pixel, start + Vec2(25.0f, 0.0f));
}

TEST_F(CalibrationUndoTest, StepsOfADeletedOrReCreatedOutputResolveToNothing) {
    nudge(0, Vec2(5.0f, 0.0f));
    points().push_back(spare_);
    session_.noteEdit("Add calibration point 8");
    session_.markChanged();
    endFrame();
    ASSERT_EQ(history_.getUndoCount(), 2u);

    // The output is deleted, and a new one takes its place (a new id).
    outputs_.outputs.erase(id_);
    const UUID newId = UUID::generate();
    outputs_.outputs[newId] = {{spare_}, ProjectorCamera{}};
    outputs_.calibrating = newId;

    EXPECT_FALSE(history_.undo());
    EXPECT_FALSE(history_.undo());
    EXPECT_FALSE(history_.canUndo());
    EXPECT_FALSE(history_.redo());
    EXPECT_FALSE(history_.redo());
    ASSERT_EQ(outputs_.outputs[newId].points.size(), 1u);  // never touched
    EXPECT_EQ(outputs_.outputs[newId].points[0].pixel, spare_.pixel);
    EXPECT_FALSE(outputs_.outputs[newId].projector.calibrated);

    // (The failed steps were consumed, so the stack never wedges; they stay in the
    // history as no-ops.) An edit recorded for a gone output is not kept, and a
    // resolver-less ref does not crash.
    EXPECT_EQ(history_.getUndoCount(), 2u);
    history_.execute(std::make_unique<CalibrationEditCommand>(OutputRef{&outputs_, id_},
                                                              CalibrationEdit{}));
    history_.execute(std::make_unique<CalibrationEditCommand>(OutputRef{}, CalibrationEdit{}));
    EXPECT_EQ(history_.getUndoCount(), 2u);
}

TEST_F(CalibrationUndoTest, RecordingDoesNotDisturbADragInProgress) {
    session_.selected = session_.dragged = 2;
    nudge(2, Vec2(3.0f, 0.0f));  // recorded
    nudge(2, Vec2(3.0f, 0.0f));  // merged
    EXPECT_EQ(session_.dragged, 2);
    EXPECT_EQ(session_.selected, 2);
    // A real undo does end the drag, and keeps the selection when the point exists.
    history_.undo();
    EXPECT_EQ(session_.dragged, -1);
    EXPECT_EQ(session_.selected, 2);
}

TEST_F(CalibrationUndoTest, OnlyNamedChangesBecomeSteps) {
    // The frame boundary re-frames an uncalibrated projector: nobody named it, no step.
    projector().pose.translation += Vec3(0.0f, 0.0f, 0.5f);
    endFrame();
    EXPECT_FALSE(history_.canUndo());

    // Named but nothing changed (a checkbox clicked twice in one frame): no step.
    session_.noteEdit("Disable calibration point 1");
    endFrame();
    EXPECT_FALSE(history_.canUndo());

    // Two different edits in one frame: one step, never merged.
    points()[0].pixel.x += 1.0f;
    session_.noteEdit("Move calibration point 1", CalibrationSession::moveKey(0));
    points()[1].enabled = false;
    session_.noteEdit("Disable calibration point 2");
    session_.markChanged();
    endFrame();
    ASSERT_EQ(history_.getUndoCount(), 1u);
    EXPECT_EQ(history_.getUndoDescription(), "Edit calibration");
    nudge(0, Vec2(1.0f, 0.0f));
    EXPECT_EQ(history_.getUndoCount(), 2u);

    // The unnamed re-frame before the first step is part of its "before", as the
    // projector really was when the edit was made.
    history_.undo();
    history_.undo();
    EXPECT_TRUE(points()[1].enabled);
}

TEST_F(CalibrationUndoTest, SelectionPastTheRestoredListIsCleared) {
    points().push_back(spare_);
    session_.selected = 7;
    session_.noteEdit("Add calibration point 8");
    session_.markChanged();
    endFrame();
    history_.undo();
    EXPECT_EQ(session_.selected, -1);
}

}  // namespace test
}  // namespace quadmapper
