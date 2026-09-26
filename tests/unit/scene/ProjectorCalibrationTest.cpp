#include <gtest/gtest.h>
#include "scene/ProjectorCalibration.hpp"
#include "scene/SmallLinAlg.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace quadmapper {
namespace test {

namespace {

constexpr int WIDTH = 1920;
constexpr int HEIGHT = 1080;

double uniform(std::mt19937& rng, double lo, double hi) {
    return std::uniform_real_distribution<double>(lo, hi)(rng);
}

// A synthetic projector: throw ratio 0.8-2.5, off-centre principal point, slightly
// non-square pixels, looking obliquely at the world origin from the +Z side with a roll.
ProjectorCamera randomCamera(std::mt19937& rng) {
    ProjectorCamera cam;
    const double throwRatio = uniform(rng, 0.8, 2.5);
    cam.intrinsics.width = WIDTH;
    cam.intrinsics.height = HEIGHT;
    cam.intrinsics.fx = static_cast<float>(WIDTH * throwRatio);
    cam.intrinsics.fy = static_cast<float>(cam.intrinsics.fx * uniform(rng, 0.99, 1.01));
    cam.intrinsics.cx = static_cast<float>(WIDTH * 0.5 + uniform(rng, -150.0, 150.0));
    cam.intrinsics.cy = static_cast<float>(HEIGHT * 0.5 + uniform(rng, -150.0, 300.0));

    // Distance at which a unit length spans about 800 pixels.
    const double distance = 2.4 * throwRatio;
    const glm::dvec3 eye = distance * glm::normalize(glm::dvec3(uniform(rng, -0.7, 0.7),
                                                                uniform(rng, -0.7, 0.7), 1.0));
    const glm::dvec3 forward = glm::normalize(-eye);
    const glm::dvec3 right = glm::normalize(glm::cross(forward, glm::dvec3(0, 1, 0)));
    const glm::dvec3 down = glm::cross(forward, right);
    glm::dmat3 rows = glm::transpose(glm::dmat3(right, down, forward));  // rows: x, y, z axes
    rows = linalg::rodriguesToMatrix(glm::dvec3(0.0, 0.0, uniform(rng, -0.3, 0.3))) * rows;  // roll
    cam.pose.rotation = glm::mat3(rows);
    cam.pose.translation = Vec3(-(rows * eye));
    return cam;
}

// World points that land inside the image. Coplanar points lie on the z = 0 plane.
std::vector<Vec3> visiblePoints(std::mt19937& rng, const ProjectorCamera& cam, int count,
                                bool coplanar) {
    std::vector<Vec3> out;
    while (static_cast<int>(out.size()) < count) {
        const Vec3 p(static_cast<float>(uniform(rng, -1.0, 1.0)),
                     static_cast<float>(uniform(rng, -1.0, 1.0)),
                     coplanar ? 0.0f : static_cast<float>(uniform(rng, -1.0, 1.0)));
        Vec2 px;
        if (cam.project(p, px) && px.x > 20 && px.x < WIDTH - 20 && px.y > 20 && px.y < HEIGHT - 20)
            out.push_back(p);
    }
    return out;
}

std::vector<CalibrationPoint> pairsFor(std::mt19937& rng, const ProjectorCamera& cam,
                                       const std::vector<Vec3>& world, double noisePx) {
    std::normal_distribution<double> noise(0.0, noisePx > 0.0 ? noisePx : 1.0);
    std::vector<CalibrationPoint> out;
    for (const Vec3& w : world) {
        CalibrationPoint p;
        p.world = w;
        cam.project(w, p.pixel);
        if (noisePx > 0.0) {
            p.pixel.x += static_cast<float>(noise(rng));
            p.pixel.y += static_cast<float>(noise(rng));
        }
        out.push_back(p);
    }
    return out;
}

double rotationError(const ProjectorCamera& a, const ProjectorCamera& b) {
    const glm::dmat3 d = glm::dmat3(a.pose.rotation) * glm::transpose(glm::dmat3(b.pose.rotation));
    return glm::length(linalg::matrixToRodrigues(d));
}

double positionError(const ProjectorCamera& a, const ProjectorCamera& b) {
    return glm::length(glm::dvec3(a.position()) - glm::dvec3(b.position()));
}

// Pixel distances between the two cameras' projections of the given points.
std::vector<double> reprojectionGaps(const ProjectorCamera& a, const ProjectorCamera& b,
                                     const std::vector<Vec3>& world) {
    std::vector<double> gaps;
    for (const Vec3& w : world) {
        Vec2 pa, pb;
        EXPECT_TRUE(a.project(w, pa));
        EXPECT_TRUE(b.project(w, pb));
        gaps.push_back(glm::length(pa - pb));
    }
    return gaps;
}

double maxReprojectionGap(const ProjectorCamera& a, const ProjectorCamera& b,
                          const std::vector<Vec3>& world) {
    const std::vector<double> gaps = reprojectionGaps(a, b, world);
    return *std::max_element(gaps.begin(), gaps.end());
}

// RMS reprojection distance of a camera over the pairs.
double rmsOf(const ProjectorCamera& cam, const std::vector<CalibrationPoint>& pts) {
    double sum = 0.0;
    for (const CalibrationPoint& p : pts) {
        Vec2 px;
        EXPECT_TRUE(cam.project(p.world, px));
        sum += glm::dot(px - p.pixel, px - p.pixel);
    }
    return std::sqrt(sum / static_cast<double>(pts.size()));
}

void expectFinite(const CalibrationResult& r) {
    const ProjectorIntrinsics& k = r.camera.intrinsics;
    EXPECT_TRUE(std::isfinite(k.fx) && std::isfinite(k.fy) && std::isfinite(k.cx) &&
                std::isfinite(k.cy));
    for (int c = 0; c < 3; ++c) {
        EXPECT_TRUE(std::isfinite(r.camera.pose.translation[c]));
        for (int row = 0; row < 3; ++row) EXPECT_TRUE(std::isfinite(r.camera.pose.rotation[c][row]));
    }
    EXPECT_TRUE(std::isfinite(r.rmsError));
    for (float e : r.perPointError) EXPECT_TRUE(std::isfinite(e));
}

}  // namespace

// ---- Small linear algebra ---------------------------------------------------------------

TEST(SmallLinAlgTest, JacobiReconstructsSymmetricMatrix) {
    std::mt19937 rng(7);
    const int n = 12;
    linalg::Matrix a(n, n);
    for (int r = 0; r < n; ++r)
        for (int c = r; c < n; ++c) a(r, c) = a(c, r) = uniform(rng, -5.0, 5.0);
    std::vector<double> values;
    linalg::Matrix v;
    ASSERT_TRUE(linalg::jacobiEigen(a, values, v));
    for (int i = 1; i < n; ++i) EXPECT_LE(values[i - 1], values[i]);
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) {
            double sum = 0.0;
            for (int k = 0; k < n; ++k) sum += v(r, k) * values[k] * v(c, k);
            EXPECT_NEAR(sum, a(r, c), 1e-10);
        }
}

TEST(SmallLinAlgTest, RqGivesUpperTriangularTimesRotation) {
    const glm::dmat3 m(glm::dvec3(2.0, -1.0, 0.5), glm::dvec3(0.3, 3.0, 1.0), glm::dvec3(-1.0, 0.2, 4.0));
    glm::dmat3 k, r;
    linalg::rq3(m, k, r);
    const glm::dmat3 back = k * r;
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row) EXPECT_NEAR(back[c][row], m[c][row], 1e-12);
    EXPECT_NEAR(glm::determinant(r), 1.0, 1e-12);
    for (int i = 0; i < 3; ++i) EXPECT_GE(k[i][i], 0.0);
    EXPECT_EQ(k[0][1], 0.0);
    EXPECT_EQ(k[0][2], 0.0);
    EXPECT_EQ(k[1][2], 0.0);
}

TEST(SmallLinAlgTest, RodriguesRoundTripsIncludingNearPi) {
    const glm::dvec3 vectors[] = {glm::dvec3(0.0), glm::dvec3(1e-9, 0, 0), glm::dvec3(0.3, -0.2, 0.5),
                                  glm::normalize(glm::dvec3(1, 2, 3)) * 3.14159,
                                  glm::normalize(glm::dvec3(-1, 0.5, 0.2)) * 3.1415926535};
    for (const glm::dvec3& v : vectors) {
        const glm::dvec3 back = linalg::matrixToRodrigues(linalg::rodriguesToMatrix(v));
        EXPECT_NEAR(glm::length(back - v), 0.0, 1e-7);
    }
}

TEST(SmallLinAlgTest, SolveLinearMatchesKnownSolution) {
    linalg::Matrix a(3, 3);
    const double values[9] = {0, 2, 1, 1, 1, 1, 4, -1, 2};  // zero first pivot needs a swap
    a.data.assign(values, values + 9);
    std::vector<double> x;
    ASSERT_TRUE(linalg::solveLinear(a, {7, 6, 8}, x));
    EXPECT_NEAR(x[0], 1.0, 1e-12);
    EXPECT_NEAR(x[1], 2.0, 1e-12);
    EXPECT_NEAR(x[2], 3.0, 1e-12);
}

// ---- Full calibration --------------------------------------------------------------------

TEST(ProjectorCalibrationTest, FullRecoversLensAndPoseExactly) {
    for (unsigned seed = 1; seed <= 12; ++seed) {
        std::mt19937 rng(seed);
        const ProjectorCamera truth = randomCamera(rng);
        const int count = 8 + static_cast<int>(seed % 13);  // 8..20
        const auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, count, false), 0.0);

        auto result = calibrateProjector(pts, ProjectorIntrinsics{}, CalibrationMode::Full);
        ASSERT_TRUE(result.isSuccess()) << "seed " << seed << ": " << result.error().message;
        const CalibrationResult& r = result.value();
        const ProjectorIntrinsics& k = r.camera.intrinsics;
        EXPECT_TRUE(r.solvedIntrinsics);
        EXPECT_TRUE(r.camera.calibrated);
        EXPECT_NEAR(k.fx, truth.intrinsics.fx, 1e-3 * truth.intrinsics.fx) << "seed " << seed;
        EXPECT_NEAR(k.fy, truth.intrinsics.fy, 1e-3 * truth.intrinsics.fy) << "seed " << seed;
        EXPECT_NEAR(k.cx, truth.intrinsics.cx, 1e-3 * truth.intrinsics.cx) << "seed " << seed;
        EXPECT_NEAR(k.cy, truth.intrinsics.cy, 1e-3 * truth.intrinsics.cy) << "seed " << seed;
        EXPECT_LT(rotationError(r.camera, truth), 1e-3) << "seed " << seed;
        EXPECT_LT(positionError(r.camera, truth), 1e-3) << "seed " << seed;
        EXPECT_LT(glm::length(r.camera.pose.translation - truth.pose.translation), 1e-3f);
        EXPECT_LT(r.rmsError, 1e-3f) << "seed " << seed;
        ASSERT_EQ(r.perPointError.size(), pts.size());
        expectFinite(r);
    }
}

TEST(ProjectorCalibrationTest, FullWithHalfPixelNoiseStaysAccurate) {
    double worstFx = 0.0, worstHeldOutRms = 0.0, worstHeldOutMax = 0.0, minRms = 1e9, maxRms = 0.0;
    for (unsigned seed = 100; seed < 112; ++seed) {
        std::mt19937 rng(seed);
        const ProjectorCamera truth = randomCamera(rng);
        const auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, 20, false), 0.5);

        auto result = calibrateProjector(pts, ProjectorIntrinsics{}, CalibrationMode::Full);
        ASSERT_TRUE(result.isSuccess()) << "seed " << seed << ": " << result.error().message;
        const CalibrationResult& r = result.value();
        const double fxError = std::abs(r.camera.intrinsics.fx - truth.intrinsics.fx) / truth.intrinsics.fx;
        EXPECT_LT(fxError, 0.02) << "seed " << seed;
        // The fit is the least-squares optimum: it explains the noisy pairs at least as
        // well as the true camera does.
        EXPECT_LE(r.rmsError, rmsOf(truth, pts) + 1e-4) << "seed " << seed;
        // Residual of 2D Gaussian noise (sigma 0.5 px per axis) after fitting 10 of 40 values.
        EXPECT_GT(r.rmsError, 0.3f) << "seed " << seed;
        EXPECT_LT(r.rmsError, 0.9f) << "seed " << seed;
        // Held-out points, measured against where the true projector puts them.
        const std::vector<double> gaps =
            reprojectionGaps(r.camera, truth, visiblePoints(rng, truth, 30, false));
        double sumSq = 0.0;
        for (double g : gaps) sumSq += g * g;
        const double heldOutRms = std::sqrt(sumSq / static_cast<double>(gaps.size()));
        const double heldOutMax = *std::max_element(gaps.begin(), gaps.end());
        EXPECT_LT(heldOutRms, 1.5) << "seed " << seed;
        EXPECT_LT(heldOutMax, 3.0) << "seed " << seed;
        expectFinite(r);

        worstFx = std::max(worstFx, fxError);
        worstHeldOutRms = std::max(worstHeldOutRms, heldOutRms);
        worstHeldOutMax = std::max(worstHeldOutMax, heldOutMax);
        minRms = std::min(minRms, static_cast<double>(r.rmsError));
        maxRms = std::max(maxRms, static_cast<double>(r.rmsError));
    }
    std::printf("[ measured ] full, noise 0.5px: worst fx error %.3f%%, RMS %.3f..%.3f px, "
                "held-out worst RMS %.3f px, worst single %.3f px\n",
                100.0 * worstFx, minRms, maxRms, worstHeldOutRms, worstHeldOutMax);
}

TEST(ProjectorCalibrationTest, RoundTripReprojectsNewPoints) {
    std::mt19937 rng(42);
    const ProjectorCamera truth = randomCamera(rng);
    const auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, 10, false), 0.0);
    auto result = calibrateProjector(pts, ProjectorIntrinsics{});
    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    EXPECT_LT(maxReprojectionGap(result.value().camera, truth, visiblePoints(rng, truth, 50, false)), 0.01);
}

// ---- Pose only ---------------------------------------------------------------------------

TEST(ProjectorCalibrationTest, PoseOnlyFromCoplanarPointsRecoversPose) {
    for (int count : {4, 6}) {
        for (unsigned seed = 200; seed < 210; ++seed) {
            std::mt19937 rng(seed);
            const ProjectorCamera truth = randomCamera(rng);
            const auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, count, true), 0.0);
            auto result = calibrateProjector(pts, truth.intrinsics, CalibrationMode::PoseOnly);
            ASSERT_TRUE(result.isSuccess()) << result.error().message;
            const CalibrationResult& r = result.value();
            EXPECT_FALSE(r.solvedIntrinsics);
            EXPECT_EQ(r.camera.intrinsics.fx, truth.intrinsics.fx);
            EXPECT_LT(rotationError(r.camera, truth), 1e-3) << count << " pts, seed " << seed;
            EXPECT_LT(positionError(r.camera, truth), 1e-3) << count << " pts, seed " << seed;
            EXPECT_LT(r.rmsError, 1e-3f);
        }
    }
}

TEST(ProjectorCalibrationTest, PoseOnlyWithNoiseStaysBounded) {
    double worstRot = 0.0, worstPos = 0.0;
    for (int count : {4, 6}) {
        for (unsigned seed = 300; seed < 310; ++seed) {
            std::mt19937 rng(seed);
            const ProjectorCamera truth = randomCamera(rng);
            const auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, count, true), 0.5);
            auto result = calibrateProjector(pts, truth.intrinsics, CalibrationMode::PoseOnly);
            ASSERT_TRUE(result.isSuccess()) << result.error().message;
            const double distance = glm::length(truth.position());
            const double rot = rotationError(result.value().camera, truth);
            const double pos = positionError(result.value().camera, truth) / distance;
            EXPECT_LT(rot, 0.02) << count << " pts, seed " << seed;
            EXPECT_LT(pos, 0.02) << count << " pts, seed " << seed;
            EXPECT_LT(result.value().rmsError, 1.5f);
            worstRot = std::max(worstRot, rot);
            worstPos = std::max(worstPos, pos);
        }
    }
    std::printf("[ measured ] pose-only coplanar, noise 0.5px: worst rotation %.4f rad, worst position %.3f%% of distance\n",
                worstRot, 100.0 * worstPos);
}

TEST(ProjectorCalibrationTest, PoseOnlyFromFewNonCoplanarPointsConverges) {
    // Five points at different depths: too few for a DLT, so the pose comes from POSIT
    // and the plane-fit homography, each refined.
    for (unsigned seed = 400; seed < 410; ++seed) {
        std::mt19937 rng(seed);
        const ProjectorCamera truth = randomCamera(rng);
        const auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, 5, false), 0.0);
        auto result = calibrateProjector(pts, truth.intrinsics, CalibrationMode::PoseOnly);
        ASSERT_TRUE(result.isSuccess()) << result.error().message;
        EXPECT_LT(rotationError(result.value().camera, truth), 1e-3) << "seed " << seed;
        EXPECT_LT(positionError(result.value().camera, truth), 1e-3) << "seed " << seed;
    }
}

TEST(ProjectorCalibrationTest, PoseOnlyFromManyNonCoplanarPoints) {
    std::mt19937 rng(500);
    const ProjectorCamera truth = randomCamera(rng);
    const auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, 12, false), 0.0);
    auto result = calibrateProjector(pts, truth.intrinsics, CalibrationMode::PoseOnly);
    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    EXPECT_FALSE(result.value().solvedIntrinsics);
    EXPECT_LT(rotationError(result.value().camera, truth), 1e-3);
    EXPECT_LT(positionError(result.value().camera, truth), 1e-3);
}

// ---- Mode selection and errors -----------------------------------------------------------

TEST(ProjectorCalibrationTest, AutoPicksFullOrPoseOnly) {
    std::mt19937 rng(600);
    const ProjectorCamera truth = randomCamera(rng);

    // 6-7 points fit the 10 lens-and-pose unknowns too loosely: Auto keeps the lens.
    auto six = calibrateProjector(pairsFor(rng, truth, visiblePoints(rng, truth, 6, false), 0.0),
                                  truth.intrinsics);
    ASSERT_TRUE(six.isSuccess());
    EXPECT_FALSE(six.value().solvedIntrinsics);

    auto general = calibrateProjector(pairsFor(rng, truth, visiblePoints(rng, truth, 8, false), 0.0),
                                      truth.intrinsics);
    ASSERT_TRUE(general.isSuccess());
    EXPECT_TRUE(general.value().solvedIntrinsics);

    auto planar = calibrateProjector(pairsFor(rng, truth, visiblePoints(rng, truth, 8, true), 0.0),
                                     truth.intrinsics);
    ASSERT_TRUE(planar.isSuccess());
    EXPECT_FALSE(planar.value().solvedIntrinsics);

    auto few = calibrateProjector(pairsFor(rng, truth, visiblePoints(rng, truth, 5, false), 0.0),
                                  truth.intrinsics);
    ASSERT_TRUE(few.isSuccess());
    EXPECT_FALSE(few.value().solvedIntrinsics);
}

TEST(ProjectorCalibrationTest, AutoNeverAppliesAnImplausibleLens) {
    // Pairs consistent with a throw ratio of 10 (outside what a projector has): the lens
    // solve recovers it, Auto refuses it and solves the pose with the known lens instead,
    // and an explicit lens solve reports it as unreliable.
    std::mt19937 rng(1400);
    ProjectorCamera truth = randomCamera(rng);
    const float scale = 10.0f * static_cast<float>(truth.intrinsics.width) / truth.intrinsics.fx;
    truth.intrinsics.fx *= scale;
    truth.intrinsics.fy *= scale;
    const auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, 12, false), 0.0);
    ProjectorIntrinsics known = truth.intrinsics;
    known.fx = known.fy = 1.5f * static_cast<float>(known.width);

    auto automatic = calibrateProjector(pts, known, CalibrationMode::Auto);
    ASSERT_TRUE(automatic.isSuccess()) << automatic.error().message;
    EXPECT_FALSE(automatic.value().solvedIntrinsics);
    EXPECT_FLOAT_EQ(automatic.value().camera.intrinsics.fx, known.fx);  // lens kept
    EXPECT_NE(automatic.value().warning.find("Lens solve looked unreliable"), std::string::npos);

    auto full = calibrateProjector(pts, known, CalibrationMode::Full);
    ASSERT_TRUE(full.isError());
    EXPECT_NE(full.error().message.find("unreliable"), std::string::npos);
}

TEST(ProjectorCalibrationTest, NudgingOneOfSixPointsKeepsThePoseStable) {
    // GUI QA: from 6 good pairs, one Shift+Right nudge sent the Auto lens to throw 0.6
    // and a principal point 9000 px off. With the lens kept, the camera barely moves.
    std::mt19937 rng(1500);
    const ProjectorCamera truth = randomCamera(rng);
    auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, 6, false), 0.0);
    for (int step = 1; step <= 5; ++step) {
        pts[2].pixel.x += 10.0f;  // a Shift+Right nudge at 1920 px wide is about 19 px
        auto result = calibrateProjector(pts, truth.intrinsics, CalibrationMode::Auto);
        ASSERT_TRUE(result.isSuccess()) << "step " << step << ": " << result.error().message;
        const ProjectorCamera& cam = result.value().camera;
        EXPECT_FLOAT_EQ(cam.intrinsics.fx, truth.intrinsics.fx);
        EXPECT_LT(glm::length(cam.position() - truth.position()), 0.5f * glm::length(truth.position()))
            << "step " << step;
    }
}

TEST(ProjectorCalibrationTest, FullWithCoplanarPointsIsRejected) {
    std::mt19937 rng(700);
    const ProjectorCamera truth = randomCamera(rng);
    auto result = calibrateProjector(pairsFor(rng, truth, visiblePoints(rng, truth, 10, true), 0.0),
                                     truth.intrinsics, CalibrationMode::Full);
    ASSERT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("one plane"), std::string::npos) << result.error().message;
}

TEST(ProjectorCalibrationTest, TooFewPointsAreRejected) {
    std::mt19937 rng(800);
    const ProjectorCamera truth = randomCamera(rng);
    auto three = calibrateProjector(pairsFor(rng, truth, visiblePoints(rng, truth, 3, false), 0.0),
                                    truth.intrinsics);
    ASSERT_TRUE(three.isError());
    EXPECT_NE(three.error().message.find("at least 4"), std::string::npos);

    auto fiveFull = calibrateProjector(pairsFor(rng, truth, visiblePoints(rng, truth, 5, false), 0.0),
                                       truth.intrinsics, CalibrationMode::Full);
    ASSERT_TRUE(fiveFull.isError());
    EXPECT_NE(fiveFull.error().message.find("at least 6"), std::string::npos);

    // Disabled points do not count.
    auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, 6, false), 0.0);
    pts[0].enabled = pts[1].enabled = pts[2].enabled = false;
    EXPECT_TRUE(calibrateProjector(pts, truth.intrinsics).isError());
}

TEST(ProjectorCalibrationTest, CollinearPointsAreRejected) {
    std::mt19937 rng(900);
    const ProjectorCamera truth = randomCamera(rng);
    std::vector<Vec3> line;
    for (int i = 0; i < 8; ++i) line.emplace_back(-0.8f + 0.2f * i, 0.1f * i, 0.05f * i);
    for (CalibrationMode mode : {CalibrationMode::Auto, CalibrationMode::PoseOnly, CalibrationMode::Full}) {
        auto result = calibrateProjector(pairsFor(rng, truth, line, 0.0), truth.intrinsics, mode);
        ASSERT_TRUE(result.isError());
        EXPECT_NE(result.error().message.find("line"), std::string::npos) << result.error().message;
    }
}

TEST(ProjectorCalibrationTest, GrossOutlierIsTheLargestPerPointError) {
    // One pair clicked 75 px off among ten. Nothing is rejected: the solve returns, the
    // good pairs still fit and the bad pair carries by far the largest error.
    for (CalibrationMode mode : {CalibrationMode::PoseOnly, CalibrationMode::Full}) {
        float lowestOutlier = 1e9f, highestOther = 0.0f;
        for (unsigned seed = 1000; seed < 1010; ++seed) {
            std::mt19937 rng(seed);
            const ProjectorCamera truth = randomCamera(rng);
            auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, 10, false), 0.3);
            const size_t bad = seed % pts.size();
            pts[bad].pixel += Vec2(60.0f, -45.0f);

            auto result = calibrateProjector(pts, truth.intrinsics, mode);
            ASSERT_TRUE(result.isSuccess()) << result.error().message;
            expectFinite(result.value());
            const auto& errors = result.value().perPointError;
            std::vector<float> others = errors;
            others.erase(others.begin() + static_cast<std::ptrdiff_t>(bad));
            const float nextLargest = *std::max_element(others.begin(), others.end());
            EXPECT_GT(errors[bad], 10.0f * nextLargest) << "seed " << seed;
            lowestOutlier = std::min(lowestOutlier, errors[bad]);
            highestOther = std::max(highestOther, nextLargest);
        }
        std::printf("[ measured ] %s, 75 px outlier: outlier error >= %.1f px, others <= %.2f px\n",
                    mode == CalibrationMode::Full ? "full" : "pose-only", lowestOutlier, highestOther);
    }
}

TEST(ProjectorCalibrationTest, TooFewPointsToLocateAMisclickWarnInstead) {
    // Below 9 pairs (solving the lens) the largest error is often an innocent pair, so the
    // result must warn rather than let the UI point at one pair.
    for (size_t n : {size_t(7), size_t(8)}) {
        for (unsigned seed = 1200; seed < 1206; ++seed) {
            std::mt19937 rng(seed);
            const ProjectorCamera truth = randomCamera(rng);
            auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, n, false), 0.3);
            pts[seed % n].pixel += Vec2(60.0f, -45.0f);
            auto result = calibrateProjector(pts, truth.intrinsics, CalibrationMode::Full);
            if (!result.isSuccess()) continue;  // a hard Err is also a safe outcome
            EXPECT_FALSE(result.value().warning.empty()) << "n=" << n << " seed " << seed;
            EXPECT_NE(result.value().warning.find("cannot be singled out"), std::string::npos);
        }
    }
}

TEST(ProjectorCalibrationTest, CleanOrLocatedMisclickHasNoWarning) {
    std::mt19937 rng(1300);
    const ProjectorCamera truth = randomCamera(rng);
    auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, 10, false), 0.3);
    auto clean = calibrateProjector(pts, truth.intrinsics, CalibrationMode::Full);
    ASSERT_TRUE(clean.isSuccess());
    EXPECT_TRUE(clean.value().warning.empty());

    pts[3].pixel += Vec2(60.0f, -45.0f);  // 10 pairs: the robust path locates it
    auto located = calibrateProjector(pts, truth.intrinsics, CalibrationMode::Full);
    ASSERT_TRUE(located.isSuccess());
    EXPECT_TRUE(located.value().warning.empty()) << located.value().warning;
    EXPECT_NE(located.value().method.find("Cauchy"), std::string::npos);
}

TEST(ProjectorCalibrationTest, DisabledPointsAreMeasuredButNotFitted) {
    std::mt19937 rng(1100);
    const ProjectorCamera truth = randomCamera(rng);
    auto pts = pairsFor(rng, truth, visiblePoints(rng, truth, 9, false), 0.0);
    pts[4].pixel += Vec2(100.0f, 0.0f);
    pts[4].enabled = false;
    auto result = calibrateProjector(pts, truth.intrinsics, CalibrationMode::Full);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_LT(result.value().rmsError, 1e-3f);
    EXPECT_NEAR(result.value().perPointError[4], 100.0f, 0.01f);
    EXPECT_EQ(result.value().camera.intrinsics.width, truth.intrinsics.width);
    EXPECT_EQ(result.value().camera.intrinsics.height, truth.intrinsics.height);
}

TEST(ProjectorCalibrationTest, NeverReturnsNaN) {
    // Unrelated world points and pixels, near-degenerate shapes and non-finite input: each
    // must either fail with a message or return finite numbers.
    for (unsigned seed = 1200; seed < 1260; ++seed) {
        std::mt19937 rng(seed);
        const int count = 4 + static_cast<int>(seed % 10);
        std::vector<CalibrationPoint> pts(static_cast<size_t>(count));
        const double flatness = (seed % 3 == 0) ? 1e-4 : 1.0;
        for (CalibrationPoint& p : pts) {
            p.world = Vec3(static_cast<float>(uniform(rng, -1, 1)), static_cast<float>(uniform(rng, -1, 1)),
                           static_cast<float>(flatness * uniform(rng, -1, 1)));
            p.pixel = Vec2(static_cast<float>(uniform(rng, 0, WIDTH)), static_cast<float>(uniform(rng, 0, HEIGHT)));
        }
        for (CalibrationMode mode : {CalibrationMode::Auto, CalibrationMode::PoseOnly, CalibrationMode::Full}) {
            auto result = calibrateProjector(pts, ProjectorIntrinsics{}, mode);
            if (result.isSuccess()) {
                expectFinite(result.value());
            } else {
                EXPECT_FALSE(result.error().message.empty());
            }
        }
    }
    std::vector<CalibrationPoint> bad(6);
    for (size_t i = 0; i < bad.size(); ++i) bad[i].world = Vec3(static_cast<float>(i), 1.0f, 0.5f * i);
    bad[2].pixel.x = std::nanf("");
    EXPECT_TRUE(calibrateProjector(bad, ProjectorIntrinsics{}).isError());
}

}  // namespace test
}  // namespace quadmapper
