#include "RayPick.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace quadmapper {

namespace {

// Snap candidates tested for occlusion, nearest on screen first. Each test casts a
// ray against every triangle, so a dense cluster under the cursor stays bounded.
constexpr size_t MAX_SNAP_VISIBILITY_TESTS = 32;

// The object's positions in world space.
std::vector<Vec3> worldPositions(const MeshObject& object) {
    const glm::mat4& m = object.getTransform();
    const auto& positions = object.getMesh().positions;
    std::vector<Vec3> world;
    world.reserve(positions.size());
    for (const Vec3& p : positions) world.emplace_back(m * glm::vec4(p, 1.0f));
    return world;
}

}  // namespace

Ray pixelRay(const ProjectorCamera& camera, const Vec2& pixel) {
    const ProjectorIntrinsics& k = camera.intrinsics;
    const Vec3 cameraDir((pixel.x - k.cx) / k.fx, (pixel.y - k.cy) / k.fy, 1.0f);
    Ray ray;
    ray.origin = camera.position();
    ray.direction = glm::normalize(glm::transpose(camera.pose.rotation) * cameraDir);
    return ray;
}

bool intersectTriangle(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c, float& t) {
    const Vec3 e1 = b - a;
    const Vec3 e2 = c - a;
    const Vec3 p = glm::cross(ray.direction, e2);
    const float det = glm::dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;  // parallel or degenerate
    const float inv = 1.0f / det;
    const Vec3 s = ray.origin - a;
    const float u = glm::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = glm::cross(s, e1);
    const float v = glm::dot(ray.direction, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = glm::dot(e2, q) * inv;
    return t > 0.0f;
}

bool pickObjects(const std::vector<std::unique_ptr<MeshObject>>& objects, const Ray& ray,
                 MeshHit& hit) {
    bool found = false;
    for (size_t o = 0; o < objects.size(); ++o) {
        const MeshObject* object = objects[o].get();
        if (!object || !object->isVisible()) continue;
        const std::vector<Vec3> world = worldPositions(*object);
        const auto& indices = object->getMesh().indices;
        for (size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
            const uint32_t i0 = indices[tri], i1 = indices[tri + 1], i2 = indices[tri + 2];
            if (i0 >= world.size() || i1 >= world.size() || i2 >= world.size()) continue;
            float t = 0.0f;
            if (intersectTriangle(ray, world[i0], world[i1], world[i2], t) &&
                (!found || t < hit.distance)) {
                found = true;
                hit.distance = t;
                hit.point = ray.origin + t * ray.direction;
                hit.objectIndex = o;
                hit.triangle = tri / 3;
            }
        }
    }
    return found;
}

bool snapToVertex(const std::vector<std::unique_ptr<MeshObject>>& objects,
                  const ProjectorCamera& camera, const Vec2& pixel, float radiusPx,
                  Vec3& vertex) {
    std::vector<std::pair<float, Vec3>> candidates;  // (screen distance, world vertex)
    for (const auto& object : objects) {
        if (!object || !object->isVisible()) continue;
        for (const Vec3& w : worldPositions(*object)) {
            Vec2 projected;
            if (!camera.project(w, projected)) continue;
            const float d = glm::length(projected - pixel);
            if (d <= radiusPx) candidates.emplace_back(d, w);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& l, const auto& r) { return l.first < r.first; });

    const Vec3 eye = camera.position();
    const size_t tests = std::min(candidates.size(), MAX_SNAP_VISIBILITY_TESTS);
    for (size_t i = 0; i < tests; ++i) {
        const Vec3& w = candidates[i].second;
        const float distance = glm::length(w - eye);
        if (distance <= 0.0f) continue;
        // Visible when nothing on the way to it is hit first (its own triangles meet the
        // ray at the vertex itself).
        const Ray ray{eye, (w - eye) / distance};
        MeshHit hit;
        if (!pickObjects(objects, ray, hit) || hit.distance >= distance * (1.0f - 1e-3f)) {
            vertex = w;
            return true;
        }
    }
    return false;
}

}  // namespace quadmapper
