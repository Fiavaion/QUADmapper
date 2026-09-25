#include <gtest/gtest.h>
#include "scene/RayPick.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <memory>
#include <vector>

namespace quadmapper {
namespace test {

namespace {

// A square of half-size `half` in the z = `z` plane of its model space, two triangles.
std::unique_ptr<MeshObject> makeSquare(float z, float half) {
    auto object = std::make_unique<MeshObject>();
    MeshData& mesh = object->getMesh();
    mesh.positions = {Vec3(-half, -half, z), Vec3(half, -half, z), Vec3(half, half, z),
                      Vec3(-half, half, z)};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return object;
}

Ray downMinusZ(float x, float y) { return Ray{Vec3(x, y, 5.0f), Vec3(0.0f, 0.0f, -1.0f)}; }

// A camera on the +Z side looking at the origin (framing a 2x2x2 box).
ProjectorCamera frontCamera() {
    return ProjectorCamera::framing(Vec3(-1.0f), Vec3(1.0f),
                                    ProjectorIntrinsics::fromThrowRatio(1.5f, 1920, 1080));
}

}  // namespace

TEST(RayPickTest, TriangleHitReportsDistance) {
    float t = 0.0f;
    ASSERT_TRUE(intersectTriangle(downMinusZ(0.1f, 0.1f), Vec3(-1, -1, 0), Vec3(1, -1, 0),
                                  Vec3(0, 1, 0), t));
    EXPECT_NEAR(t, 5.0f, 1e-5f);
}

TEST(RayPickTest, TriangleIsTwoSided) {
    float t = 0.0f;
    // Same triangle wound the other way.
    EXPECT_TRUE(intersectTriangle(downMinusZ(0.1f, 0.1f), Vec3(0, 1, 0), Vec3(1, -1, 0),
                                  Vec3(-1, -1, 0), t));
}

TEST(RayPickTest, TriangleMissesOutsideBehindAndParallel) {
    float t = 0.0f;
    const Vec3 a(-1, -1, 0), b(1, -1, 0), c(0, 1, 0);
    EXPECT_FALSE(intersectTriangle(downMinusZ(2.0f, 0.0f), a, b, c, t));  // outside
    EXPECT_FALSE(intersectTriangle(Ray{Vec3(0, 0, -5), Vec3(0, 0, -1)}, a, b, c, t));  // behind
    EXPECT_FALSE(intersectTriangle(Ray{Vec3(-5, 0, 0), Vec3(1, 0, 0)}, a, b, c, t));  // in-plane
}

TEST(RayPickTest, PickReturnsNearestHitAcrossObjects) {
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(makeSquare(-1.0f, 1.0f));  // farther, listed first
    objects.push_back(makeSquare(0.5f, 1.0f));   // nearer
    MeshHit hit;
    ASSERT_TRUE(pickObjects(objects, downMinusZ(0.2f, -0.3f), hit));
    EXPECT_EQ(hit.objectIndex, 1u);
    EXPECT_NEAR(hit.distance, 4.5f, 1e-5f);
    EXPECT_NEAR(hit.point.x, 0.2f, 1e-5f);
    EXPECT_NEAR(hit.point.y, -0.3f, 1e-5f);
    EXPECT_NEAR(hit.point.z, 0.5f, 1e-5f);
}

TEST(RayPickTest, PickSkipsHiddenObjectsAndMisses) {
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(makeSquare(0.5f, 1.0f));
    objects.push_back(makeSquare(-1.0f, 1.0f));
    objects[0]->setVisible(false);
    MeshHit hit;
    ASSERT_TRUE(pickObjects(objects, downMinusZ(0.0f, 0.0f), hit));
    EXPECT_EQ(hit.objectIndex, 1u);
    EXPECT_FALSE(pickObjects(objects, downMinusZ(3.0f, 0.0f), hit));
}

TEST(RayPickTest, PickUsesTheObjectTransform) {
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(makeSquare(0.0f, 1.0f));
    // Scale 2 then move to x = 10, z = 1: the square covers x in [8, 12] at z = 1.
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 1.0f));
    objects[0]->setTransform(glm::scale(m, glm::vec3(2.0f)));
    MeshHit hit;
    EXPECT_FALSE(pickObjects(objects, downMinusZ(0.0f, 0.0f), hit));
    ASSERT_TRUE(pickObjects(objects, downMinusZ(11.5f, 1.5f), hit));
    EXPECT_NEAR(hit.point.z, 1.0f, 1e-5f);
    EXPECT_NEAR(hit.distance, 4.0f, 1e-5f);
}

TEST(RayPickTest, PixelRayPassesThroughTheProjectedPoint) {
    const ProjectorCamera camera = frontCamera();
    const Vec3 world(0.4f, -0.7f, 0.3f);
    Vec2 pixel;
    ASSERT_TRUE(camera.project(world, pixel));
    const Ray ray = pixelRay(camera, pixel);
    EXPECT_NEAR(glm::length(ray.direction), 1.0f, 1e-5f);
    const float t = glm::dot(world - ray.origin, ray.direction);
    EXPECT_LT(glm::length(ray.origin + t * ray.direction - world), 1e-4f);
}

TEST(RayPickTest, PixelRayPicksWhatTheCameraShows) {
    const ProjectorCamera camera = frontCamera();
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(makeSquare(0.0f, 1.0f));
    Vec2 pixel;
    ASSERT_TRUE(camera.project(Vec3(0.25f, 0.5f, 0.0f), pixel));
    MeshHit hit;
    ASSERT_TRUE(pickObjects(objects, pixelRay(camera, pixel), hit));
    EXPECT_LT(glm::length(hit.point - Vec3(0.25f, 0.5f, 0.0f)), 1e-4f);
}

TEST(RayPickTest, SnapFindsTheNearestVertexWithinRadius) {
    const ProjectorCamera camera = frontCamera();
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(makeSquare(0.0f, 1.0f));
    Vec2 corner;
    ASSERT_TRUE(camera.project(Vec3(1.0f, 1.0f, 0.0f), corner));

    Vec3 vertex;
    ASSERT_TRUE(snapToVertex(objects, camera, corner + Vec2(-6.0f, 5.0f), 10.0f, vertex));
    EXPECT_EQ(vertex, Vec3(1.0f, 1.0f, 0.0f));
    EXPECT_FALSE(snapToVertex(objects, camera, corner + Vec2(-30.0f, 30.0f), 10.0f, vertex));
}

TEST(RayPickTest, SnapIgnoresVerticesHiddenBehindASurface) {
    const ProjectorCamera camera = frontCamera();
    std::vector<std::unique_ptr<MeshObject>> objects;
    objects.push_back(makeSquare(-0.5f, 0.2f));  // small, behind
    objects.push_back(makeSquare(0.5f, 1.0f));   // large, in front, covers it
    Vec2 hidden;
    ASSERT_TRUE(camera.project(Vec3(0.2f, 0.2f, -0.5f), hidden));
    Vec3 vertex;
    EXPECT_FALSE(snapToVertex(objects, camera, hidden, 10.0f, vertex));

    objects[1]->setVisible(false);  // uncovered: now it snaps
    ASSERT_TRUE(snapToVertex(objects, camera, hidden, 10.0f, vertex));
    EXPECT_EQ(vertex, Vec3(0.2f, 0.2f, -0.5f));
}

}  // namespace test
}  // namespace quadmapper
