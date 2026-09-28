#pragma once

// A projector modelled as an inverse pinhole camera (ADR-052). Conventions follow
// OpenCV so calibration output is used as-is: image pixels have their origin at the
// top-left with +y down; the camera looks down +Z with +X right and +Y down.

#include "core/Types.hpp"

#include <glm/glm.hpp>

namespace quadmapper {

struct ProjectorIntrinsics {
    float fx = 1920.0f * 1.5f;  // focal lengths in pixels
    float fy = 1920.0f * 1.5f;
    float cx = 960.0f;          // principal point in pixels
    float cy = 540.0f;
    int width = 1920;           // image (output) size the values refer to
    int height = 1080;

    // Square-pixel lens from its throw ratio (distance / image width). lensShift moves
    // the principal point vertically by that fraction of the image height (0 = centred,
    // 0.5 = on the bottom edge, as for a projector mounted below its image).
    static ProjectorIntrinsics fromThrowRatio(float throwRatio, int width, int height,
                                              float lensShift = 0.0f);
};

// World -> camera: pc = rotation * world + translation.
struct ProjectorPose {
    glm::mat3 rotation{1.0f};
    Vec3 translation{0.0f};
};

// A 2D<->3D correspondence for calibration: a world point and where it must land on
// the projector image, in pixels.
struct CalibrationPoint {
    Vec3 world{0.0f};
    Vec2 pixel{0.0f};
    bool enabled = true;
};

struct ProjectorCamera {
    ProjectorIntrinsics intrinsics;
    ProjectorPose pose;
    bool calibrated = false;

    // The reference model. False when the point is not in front of the projector.
    bool project(const Vec3& world, Vec2& pixel) const;

    // OpenGL equivalents of project(): world -> eye (GL convention) and eye -> clip.
    // Rendering with projection * view places every point exactly where project()
    // says, with NDC y = +1 at the top image row.
    glm::mat4 viewMatrix() const;
    glm::mat4 projectionMatrix(float nearZ, float farZ) const;

    // Where the projector sits and which way it points, in world space.
    Vec3 position() const;

    // An uncalibrated default: this lens, standing on the world +Z side of the box,
    // looking down -Z (world +Y up), far enough back that the whole box is in view.
    static ProjectorCamera framing(const Vec3& boxMin, const Vec3& boxMax,
                                   const ProjectorIntrinsics& intrinsics);

    // The camera an output renders and calibrates through, given its current
    // (rotation-aware) image size. A calibrated camera keeps its pose and has its lens
    // rescaled if the size changed; an uncalibrated one keeps its throw ratio and lens
    // shift and, when the scene has objects (bounds non-null), is re-framed on them, so
    // objects stay in view whatever was imported, assigned or rotated since.
    static ProjectorCamera fitToOutput(const ProjectorCamera& stored, int width, int height,
                                       const Vec3* boundsMin, const Vec3* boundsMax);
};

}  // namespace quadmapper
