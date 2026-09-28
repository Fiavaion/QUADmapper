#pragma once

// Picking a 3D point on the scene's objects by casting a ray through a camera pixel
// (ADR-052 §6, the calibration model view). Pure geometry, no GL.

#include "MeshObject.hpp"
#include "ProjectorCamera.hpp"
#include "core/Types.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace quadmapper {

struct Ray {
    Vec3 origin{0.0f};
    Vec3 direction{0.0f, 0.0f, 1.0f};  // need not be unit length; t is in its units
};

// The ray from the camera centre through `pixel` (OpenCV convention, top-left origin).
// The direction is unit length, so hit distances are world units.
Ray pixelRay(const ProjectorCamera& camera, const Vec2& pixel);

// Two-sided Moller-Trumbore. True with the ray parameter t > 0 when the ray crosses the
// triangle (a, b, c).
bool intersectTriangle(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c, float& t);

struct MeshHit {
    Vec3 point{0.0f};        // world space
    float distance = 0.0f;   // ray parameter of the hit
    size_t objectIndex = 0;  // into the objects list
    size_t triangle = 0;
};

// Nearest hit of the ray on the visible objects' world-space triangles.
bool pickObjects(const std::vector<std::unique_ptr<MeshObject>>& objects, const Ray& ray,
                 MeshHit& hit);

// The visible objects' mesh vertex that `camera` shows nearest to `pixel`, within
// radiusPx on screen, and that no triangle hides from the camera. False when none.
bool snapToVertex(const std::vector<std::unique_ptr<MeshObject>>& objects,
                  const ProjectorCamera& camera, const Vec2& pixel, float radiusPx,
                  Vec3& vertex);

}  // namespace quadmapper
