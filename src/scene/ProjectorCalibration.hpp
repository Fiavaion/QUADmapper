#pragma once

// Recovers a physical projector's lens and pose from point pairs (ADR-052 §4): each pair
// is a 3D point on the mesh and the projector pixel where that point must be lit. This is
// the camSchnappr / MAPAMOK method: a linear estimate (DLT or plane homography) refined by
// Levenberg-Marquardt on the reprojection error. Skew and lens distortion are not modelled.

#include "ProjectorCamera.hpp"
#include "core/Result.hpp"

#include <string>
#include <vector>

namespace quadmapper {

enum class CalibrationMode {
    Auto,      // Full when there are >= 6 enabled points not on one plane, else PoseOnly
    PoseOnly,  // keep the known lens, solve the pose (>= 4 points, coplanar allowed)
    Full       // solve fx, fy, cx, cy and the pose (>= 6 points, not coplanar)
};

struct CalibrationResult {
    ProjectorCamera camera;  // calibrated = true; width/height from knownIntrinsics
    float rmsError = 0.0f;   // RMS reprojection distance over enabled points, in pixels
    // Reprojection distance in pixels for every input point, indexed like the input.
    // Disabled points are measured too (a held-out check); -1 when a point is behind
    // the projector or its coordinates are not finite.
    std::vector<float> perPointError;
    bool solvedIntrinsics = false;
    std::string method;  // human-readable description of the path taken
    // Non-empty when the fit is poor (worst pair > 3 px) and no single mismatched pair
    // could be identified: with too few points (fewer than 9 when solving the lens, 6
    // for pose only) the largest per-point error need not be the wrong pair, so the UI
    // must show this instead of pointing at one pair.
    std::string warning;
};

// Only points with `enabled` set take part in the solve. When the worst pair is more than
// 3 px off and there are enough points (9+ solving the lens, 6+ pose only), a single
// mismatched pair is located and the fit is re-weighted around it (Cauchy loss); it keeps
// its large perPointError for the user to fix or disable. Otherwise `warning` says the
// bad pair cannot be told apart. Nothing is ever dropped.
Result<CalibrationResult, Error> calibrateProjector(const std::vector<CalibrationPoint>& points,
                                                   const ProjectorIntrinsics& knownIntrinsics,
                                                   CalibrationMode mode = CalibrationMode::Auto);

}  // namespace quadmapper
