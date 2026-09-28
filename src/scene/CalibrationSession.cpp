#include "CalibrationSession.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace quadmapper {

namespace {
constexpr float ORBIT_FOV_Y_DEG = 45.0f;
constexpr float ORBIT_MAX_PITCH_DEG = 89.0f;
}  // namespace

ProjectorCamera orbitCamera(const Vec3& target, float yawDeg, float pitchDeg, float distance,
                            int width, int height) {
    ProjectorCamera camera;
    ProjectorIntrinsics& k = camera.intrinsics;
    k.width = std::max(width, 1);
    k.height = std::max(height, 1);
    k.fy = 0.5f * static_cast<float>(k.height) / std::tan(glm::radians(0.5f * ORBIT_FOV_Y_DEG));
    k.fx = k.fy;
    k.cx = 0.5f * static_cast<float>(k.width);
    k.cy = 0.5f * static_cast<float>(k.height);

    const float yaw = glm::radians(yawDeg);
    const float pitch =
        glm::radians(std::clamp(pitchDeg, -ORBIT_MAX_PITCH_DEG, ORBIT_MAX_PITCH_DEG));
    const Vec3 eye = target + distance * Vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                              std::cos(pitch) * std::cos(yaw));
    // Camera axes (OpenCV): z forward to the target, x right, y down (world +Y is up).
    const Vec3 z = glm::normalize(target - eye);
    const Vec3 x = glm::normalize(glm::cross(z, Vec3(0.0f, 1.0f, 0.0f)));
    const Vec3 y = glm::cross(z, x);
    camera.pose.rotation = glm::transpose(glm::mat3(x, y, z));  // rows x, y, z
    camera.pose.translation = -(camera.pose.rotation * eye);
    return camera;
}

int requiredCalibrationPoints(CalibrationMode mode) {
    return mode == CalibrationMode::Full ? 6 : 4;
}

float reprojectionError(const CalibrationPoint& point, const ProjectorCamera& camera) {
    Vec2 projected;
    if (!camera.project(point.world, projected)) return -1.0f;
    return glm::length(projected - point.pixel);
}

void CalibrationSession::begin(const std::vector<CalibrationPoint>& points,
                               const ProjectorCamera& projector) {
    lens = projector.intrinsics;
    status.clear();
    statusIsError = false;
    solved = false;
    rmsError = 0.0f;
    pointsUsed = 0;
    selected = -1;
    cancelPick();
    dirty_ = true;
    editDescription_.clear();
    editMergeKey_ = 0;
    base_ = capture(points, projector);
}

void CalibrationSession::noteEdit(const std::string& description, int mergeKey) {
    if (!editDescription_.empty()) {
        // Two different edits in one frame are one step with no merge.
        if (editDescription_ != description || editMergeKey_ != mergeKey) {
            editDescription_ = "Edit calibration";
            editMergeKey_ = 0;
        }
        return;
    }
    editDescription_ = description.empty() ? "Edit calibration" : description;
    editMergeKey_ = mergeKey;
}

CalibrationSnapshot CalibrationSession::capture(const std::vector<CalibrationPoint>& points,
                                                const ProjectorCamera& projector) const {
    CalibrationSnapshot s;
    s.points = points;
    s.projector = projector;
    s.lens = lens;
    s.mode = mode;
    s.status = status;
    s.statusIsError = statusIsError;
    s.warning = warning;
    s.solved = solved;
    s.rmsError = rmsError;
    s.pointsUsed = pointsUsed;
    return s;
}

std::optional<CalibrationEdit> CalibrationSession::finishFrame(
        const std::vector<CalibrationPoint>& points, const ProjectorCamera& projector) {
    CalibrationSnapshot now = capture(points, projector);
    std::optional<CalibrationEdit> edit;
    if (!editDescription_.empty() && base_ && !sameCalibrationState(*base_, now)) {
        edit = CalibrationEdit{std::move(*base_), now, editDescription_, editMergeKey_};
    }
    base_ = std::move(now);
    editDescription_.clear();
    editMergeKey_ = 0;
    return edit;
}

void CalibrationSession::restore(const CalibrationSnapshot& snapshot,
                                 std::vector<CalibrationPoint>& points, ProjectorCamera& projector) {
    points = snapshot.points;
    projector = snapshot.projector;
    lens = snapshot.lens;
    mode = snapshot.mode;
    status = snapshot.status;
    statusIsError = snapshot.statusIsError;
    warning = snapshot.warning;
    solved = snapshot.solved;
    rmsError = snapshot.rmsError;
    pointsUsed = snapshot.pointsUsed;
    if (selected >= static_cast<int>(points.size())) selected = -1;
    cancelPick();
    dirty_ = false;
    editDescription_.clear();
    editMergeKey_ = 0;
    base_ = snapshot;
}

namespace {

bool sameIntrinsics(const ProjectorIntrinsics& a, const ProjectorIntrinsics& b) {
    return a.fx == b.fx && a.fy == b.fy && a.cx == b.cx && a.cy == b.cy && a.width == b.width &&
           a.height == b.height;
}

}  // namespace

bool sameCalibrationState(const CalibrationSnapshot& a, const CalibrationSnapshot& b) {
    if (a.points.size() != b.points.size() || a.mode != b.mode || !sameIntrinsics(a.lens, b.lens) ||
        !sameIntrinsics(a.projector.intrinsics, b.projector.intrinsics) ||
        a.projector.calibrated != b.projector.calibrated ||
        a.projector.pose.rotation != b.projector.pose.rotation ||
        a.projector.pose.translation != b.projector.pose.translation) {
        return false;
    }
    for (size_t i = 0; i < a.points.size(); ++i) {
        const CalibrationPoint& p = a.points[i];
        const CalibrationPoint& q = b.points[i];
        if (p.world != q.world || p.pixel != q.pixel || p.enabled != q.enabled) return false;
    }
    return true;
}

void CalibrationSession::removePoint(std::vector<CalibrationPoint>& points, int index) {
    if (index < 0 || index >= static_cast<int>(points.size())) return;
    points.erase(points.begin() + index);
    if (selected == index) {
        selected = -1;
    } else if (selected > index) {
        --selected;
    }
    dragged = -1;
    markChanged();
}

bool CalibrationSession::update(const std::vector<CalibrationPoint>& points,
                                ProjectorCamera& projector) {
    if (!dirty_) return false;
    dirty_ = false;

    pointsUsed = static_cast<int>(
        std::count_if(points.begin(), points.end(), [](const CalibrationPoint& p) { return p.enabled; }));
    const int needed = requiredCalibrationPoints(mode);
    if (pointsUsed < needed) {
        solved = false;
        statusIsError = false;
        rmsError = 0.0f;
        warning.clear();
        const int more = needed - pointsUsed;
        status = "Add " + std::to_string(more) + " more enabled point" + (more == 1 ? "" : "s") +
                 " to solve (" + std::to_string(needed) + " needed)";
        return false;
    }

    warning.clear();
    auto result = calibrateProjector(points, lens, mode);
    if (!result) {
        solved = false;
        statusIsError = true;
        status = result.error().message;
        return false;
    }
    const CalibrationResult& solution = result.value();
    projector = solution.camera;
    projector.calibrated = true;
    if (solution.solvedIntrinsics) lens = solution.camera.intrinsics;
    solved = true;
    statusIsError = false;
    rmsError = solution.rmsError;
    status = solution.method;
    warning = solution.warning;
    return true;
}

}  // namespace quadmapper
