#include <gtest/gtest.h>
#include "scene/ProjectorCamera.hpp"
#include "scene/MeshObject.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace quadmapper {
namespace test {

namespace {

// A camera that is not axis-aligned, with an off-centre principal point, so every
// term of the matrices is exercised.
ProjectorCamera obliqueCamera() {
    ProjectorCamera cam;
    cam.intrinsics.fx = 2100.0f;
    cam.intrinsics.fy = 2050.0f;
    cam.intrinsics.cx = 1010.0f;
    cam.intrinsics.cy = 700.0f;
    cam.intrinsics.width = 1920;
    cam.intrinsics.height = 1080;
    const glm::mat4 r = glm::rotate(glm::mat4(1.0f), 0.4f, glm::vec3(0.3f, 1.0f, 0.1f));
    cam.pose.rotation = glm::mat3(r);
    cam.pose.translation = Vec3(0.2f, -0.1f, 4.0f);
    return cam;
}

// Pixel position the OpenGL pipeline produces for a world point.
Vec2 glPixel(const ProjectorCamera& cam, const Vec3& world) {
    const glm::vec4 clip =
        cam.projectionMatrix(0.1f, 100.0f) * cam.viewMatrix() * glm::vec4(world, 1.0f);
    const Vec3 ndc = Vec3(clip) / clip.w;
    return Vec2((ndc.x + 1.0f) * 0.5f * cam.intrinsics.width,
                (1.0f - ndc.y) * 0.5f * cam.intrinsics.height);
}

}  // namespace

TEST(ProjectorCameraTest, OpenGLMatricesReproduceTheReferenceProjection) {
    // The contract every render path relies on: drawing with projection * view puts
    // each point exactly where project() says the projector will light it.
    const ProjectorCamera cam = obliqueCamera();
    const Vec3 points[] = {Vec3(0, 0, 0), Vec3(0.5f, 0.3f, -0.2f), Vec3(-0.7f, -0.4f, 0.6f),
                           Vec3(1.0f, 1.0f, 1.0f), Vec3(-1.2f, 0.8f, -0.9f)};
    for (const Vec3& p : points) {
        Vec2 reference;
        ASSERT_TRUE(cam.project(p, reference));
        const Vec2 gl = glPixel(cam, p);
        EXPECT_NEAR(gl.x, reference.x, 1e-2f);
        EXPECT_NEAR(gl.y, reference.y, 1e-2f);
    }
}

TEST(ProjectorCameraTest, PointBehindTheProjectorDoesNotProject) {
    const ProjectorCamera cam = obliqueCamera();
    Vec2 pixel;
    EXPECT_FALSE(cam.project(cam.position() - 5.0f * (glm::transpose(cam.pose.rotation) * Vec3(0, 0, 1)),
                             pixel));
}

TEST(ProjectorCameraTest, ThrowRatioGivesFocalLengthAndShift) {
    const auto k = ProjectorIntrinsics::fromThrowRatio(1.5f, 1920, 1080, 0.5f);
    EXPECT_FLOAT_EQ(k.fx, 2880.0f);
    EXPECT_FLOAT_EQ(k.fy, 2880.0f);
    EXPECT_FLOAT_EQ(k.cx, 960.0f);
    EXPECT_FLOAT_EQ(k.cy, 1080.0f);  // principal point on the bottom edge

    // A 1 m wide image at 1.5 m: a point 0.5 m right of the axis lands on the edge.
    ProjectorCamera cam;
    cam.intrinsics = ProjectorIntrinsics::fromThrowRatio(1.5f, 1920, 1080);
    Vec2 px;
    ASSERT_TRUE(cam.project(Vec3(0.5f, 0.0f, 1.5f), px));
    EXPECT_NEAR(px.x, 1920.0f, 1e-3f);
}

TEST(ProjectorCameraTest, FramingPutsTheWholeBoxInViewUpright) {
    const Vec3 lo(-0.5f, 0.0f, -0.3f), hi(0.5f, 1.2f, 0.3f);
    const ProjectorCamera cam =
        ProjectorCamera::framing(lo, hi, ProjectorIntrinsics::fromThrowRatio(1.5f, 1920, 1080));
    EXPECT_FALSE(cam.calibrated);
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        Vec2 px;
        ASSERT_TRUE(cam.project(corner, px)) << "corner " << i;
        EXPECT_GE(px.x, 0.0f);
        EXPECT_LE(px.x, 1920.0f);
        EXPECT_GE(px.y, 0.0f);
        EXPECT_LE(px.y, 1080.0f);
    }
    // World +Y is up in the image (smaller pixel y).
    Vec2 low, high;
    ASSERT_TRUE(cam.project(Vec3(0, 0, 0), low));
    ASSERT_TRUE(cam.project(Vec3(0, 1, 0), high));
    EXPECT_LT(high.y, low.y);
}

TEST(MeshObjectTest, WorldBoundsFollowTheTransform) {
    MeshObject object;
    object.getMesh().positions = {Vec3(0, 0, 0), Vec3(1, 2, 3)};
    object.setTransform(glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)));
    Vec3 lo, hi;
    object.worldBounds(lo, hi);
    EXPECT_EQ(lo, Vec3(10, 0, 0));
    EXPECT_EQ(hi, Vec3(11, 2, 3));
}

}  // namespace test
}  // namespace quadmapper
