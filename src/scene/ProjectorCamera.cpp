#include "ProjectorCamera.hpp"

#include <algorithm>
#include <cmath>

namespace quadmapper {

ProjectorIntrinsics ProjectorIntrinsics::fromThrowRatio(float throwRatio, int width, int height,
                                                        float lensShift) {
    ProjectorIntrinsics k;
    k.width = width;
    k.height = height;
    // throw ratio = distance / image width, so the focal length is width * ratio pixels.
    k.fx = k.fy = static_cast<float>(width) * throwRatio;
    k.cx = 0.5f * static_cast<float>(width);
    k.cy = (0.5f + lensShift) * static_cast<float>(height);
    return k;
}

bool ProjectorCamera::project(const Vec3& world, Vec2& pixel) const {
    const Vec3 pc = pose.rotation * world + pose.translation;
    if (pc.z <= 1e-6f) return false;
    pixel = Vec2(intrinsics.fx * pc.x / pc.z + intrinsics.cx,
                 intrinsics.fy * pc.y / pc.z + intrinsics.cy);
    return true;
}

glm::mat4 ProjectorCamera::viewMatrix() const {
    // [R | t] in the OpenCV camera frame, then flip y and z into the GL eye frame
    // (y up, looking down -z).
    glm::mat4 rt(pose.rotation);
    rt[3] = glm::vec4(pose.translation, 1.0f);
    const glm::mat4 cvToGl = glm::mat4(glm::vec4(1, 0, 0, 0), glm::vec4(0, -1, 0, 0),
                                       glm::vec4(0, 0, -1, 0), glm::vec4(0, 0, 0, 1));
    return cvToGl * rt;
}

glm::mat4 ProjectorCamera::projectionMatrix(float nearZ, float farZ) const {
    const float w = static_cast<float>(intrinsics.width);
    const float h = static_cast<float>(intrinsics.height);
    // Rows derived from u = fx*x/z + cx, v = fy*y/z + cy with (x, y, z) = (xe, -ye, -ze)
    // and NDC x = 2u/w - 1, NDC y = 1 - 2v/h. glm is column-major: m[col][row].
    glm::mat4 m(0.0f);
    m[0][0] = 2.0f * intrinsics.fx / w;
    m[2][0] = 1.0f - 2.0f * intrinsics.cx / w;
    m[1][1] = 2.0f * intrinsics.fy / h;
    m[2][1] = 2.0f * intrinsics.cy / h - 1.0f;
    m[2][2] = -(farZ + nearZ) / (farZ - nearZ);
    m[3][2] = -2.0f * farZ * nearZ / (farZ - nearZ);
    m[2][3] = -1.0f;
    return m;
}

Vec3 ProjectorCamera::position() const {
    return -glm::transpose(pose.rotation) * pose.translation;
}

ProjectorCamera ProjectorCamera::framing(const Vec3& boxMin, const Vec3& boxMax,
                                         const ProjectorIntrinsics& intrinsics) {
    ProjectorCamera camera;
    camera.intrinsics = intrinsics;

    const Vec3 centre = 0.5f * (boxMin + boxMax);
    const float radius = std::max(0.5f * glm::length(boxMax - boxMin), 1e-3f);
    // The bounding sphere must fit inside the smaller half-extent of the image.
    const float halfExtent = 0.5f * static_cast<float>(std::min(intrinsics.width, intrinsics.height));
    const float focal = std::max(intrinsics.fx, intrinsics.fy);
    const float distance = 1.2f * radius * focal / halfExtent + radius;

    // Camera x = world +X, camera y (down) = world -Y, camera z (forward) = world -Z.
    camera.pose.rotation = glm::mat3(glm::vec3(1, 0, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, -1));
    const Vec3 eye = centre + Vec3(0.0f, 0.0f, distance);
    camera.pose.translation = -(camera.pose.rotation * eye);
    camera.calibrated = false;
    return camera;
}

ProjectorCamera ProjectorCamera::fitToOutput(const ProjectorCamera& stored, int width, int height,
                                             const Vec3* boundsMin, const Vec3* boundsMax) {
    if (width <= 0 || height <= 0) return stored;
    ProjectorCamera fitted = stored;
    const ProjectorIntrinsics& k = stored.intrinsics;
    if (k.width != width || k.height != height) {
        if (stored.calibrated && k.width > 0 && k.height > 0) {
            const float sx = static_cast<float>(width) / static_cast<float>(k.width);
            const float sy = static_cast<float>(height) / static_cast<float>(k.height);
            fitted.intrinsics.fx *= sx;
            fitted.intrinsics.cx *= sx;
            fitted.intrinsics.fy *= sy;
            fitted.intrinsics.cy *= sy;
            fitted.intrinsics.width = width;
            fitted.intrinsics.height = height;
        } else {
            const float throwRatio = k.width > 0 ? k.fx / static_cast<float>(k.width) : 1.5f;
            const float shift = k.height > 0 ? k.cy / static_cast<float>(k.height) - 0.5f : 0.0f;
            fitted.intrinsics = ProjectorIntrinsics::fromThrowRatio(throwRatio, width, height, shift);
        }
    }
    if (!stored.calibrated && boundsMin && boundsMax) {
        fitted = framing(*boundsMin, *boundsMax, fitted.intrinsics);
    }
    return fitted;
}

}  // namespace quadmapper
