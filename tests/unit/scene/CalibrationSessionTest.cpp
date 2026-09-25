#include <gtest/gtest.h>
#include "scene/CalibrationSession.hpp"

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
    session.begin(projector);
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
    session.begin(projector);
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
    session.begin(projector);
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

TEST(CalibrationSessionTest, SolvesOnlyAfterAChange) {
    const auto points = pairsFor(trueCamera(), cubeCorners());
    ProjectorCamera projector;
    projector.intrinsics = throwLens();
    CalibrationSession session;
    session.begin(projector);
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
    session.begin(projector);
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

}  // namespace test
}  // namespace quadmapper
