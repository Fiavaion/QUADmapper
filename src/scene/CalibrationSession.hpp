#pragma once

// The editing state and live-solve policy behind projector calibration (ADR-052 §6):
// the user pairs 3D points on the model with pixels on the editor canvas, and every
// change re-solves the output's projector. The UI (CalibrationPanel, ViewportEditor)
// only draws and forwards input; the rules live here, where tests reach them.

#include "ProjectorCalibration.hpp"
#include "ProjectorCamera.hpp"
#include "core/Types.hpp"

#include <string>
#include <vector>

namespace quadmapper {

// The editor canvas's view transform (ADR-041): canvas [0,1] is scaled and offset into
// the canvas region, both in region-normalised [0,1] units.
struct CanvasView {
    Vec2 scale{1.0f};
    Vec2 offset{0.0f};

    Vec2 toCanvas(const Vec2& regionNorm) const { return (regionNorm - offset) / scale; }
    Vec2 toRegion(const Vec2& canvas) const { return canvas * scale + offset; }
};

// The canvas shows the projector image edge to edge: canvas [0,1] <-> [0,width]x[0,height].
inline Vec2 canvasToPixel(const Vec2& canvas, const ProjectorIntrinsics& k) {
    return Vec2(canvas.x * static_cast<float>(k.width), canvas.y * static_cast<float>(k.height));
}
inline Vec2 pixelToCanvas(const Vec2& pixel, const ProjectorIntrinsics& k) {
    return Vec2(pixel.x / static_cast<float>(k.width), pixel.y / static_cast<float>(k.height));
}

// A turntable camera for the model view: width x height pixels with a 45-degree vertical
// field of view, `distance` from `target`, at `yawDeg` around world +Y (0 = on the +Z
// side) and `pitchDeg` above the horizon (clamped short of the poles). World +Y is up.
ProjectorCamera orbitCamera(const Vec3& target, float yawDeg, float pitchDeg, float distance,
                            int width, int height);

// Enabled points a solve needs: 6 to solve the lens, 4 otherwise.
int requiredCalibrationPoints(CalibrationMode mode);

// How far `camera` lands the point's world position from its target pixel, in pixels;
// -1 when the point is behind the camera. For the camera a solve returned this equals
// CalibrationResult::perPointError, and it stays right after later edits.
float reprojectionError(const CalibrationPoint& point, const ProjectorCamera& camera);

struct CalibrationSession {
    enum class Pick {
        None,
        World,  // Add Point: waiting for a click on the model
        Pixel   // waiting for a click on the canvas where pendingWorld must land
    };

    // The lens a pose-only solve keeps: set from the throw ratio, or from a solve
    // that recovered the lens. Its width/height are the projector image's.
    ProjectorIntrinsics lens;
    CalibrationMode mode = CalibrationMode::Auto;

    // Result of the last solve attempt. `status` is the solver's message verbatim on
    // failure.
    std::string status;
    bool statusIsError = false;
    bool solved = false;  // the last attempt succeeded
    float rmsError = 0.0f;
    int pointsUsed = 0;

    // Interaction state.
    int selected = -1;
    Pick pick = Pick::None;
    Vec3 pendingWorld{0.0f};
    int dragged = -1;
    Vec2 dragStartMousePixel{0.0f};
    Vec2 dragStartPixel{0.0f};

    // Start (or restart) on a projector: its lens, nothing selected, and a solve on the
    // next update so the status reflects the stored points.
    void begin(const ProjectorCamera& projector);

    // Drop any pick or drag in progress.
    void cancelPick() {
        pick = Pick::None;
        dragged = -1;
    }

    // Something the solve depends on changed (points, lens, mode).
    void markChanged() { dirty_ = true; }
    bool isDirty() const { return dirty_; }

    // Remove a point and keep the selection on the same point (or none).
    void removePoint(std::vector<CalibrationPoint>& points, int index);

    // The live solve. Call once a frame: when something changed since the last call
    // and enough points are enabled, solve and, on success, replace `projector`
    // (calibrated) with the result. On failure the projector keeps its previous camera
    // and `status` holds the reason. Returns true when the projector was replaced.
    bool update(const std::vector<CalibrationPoint>& points, ProjectorCamera& projector);

private:
    bool dirty_ = false;
};

}  // namespace quadmapper
