#pragma once

#include "../core/UUID.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace quadmapper {

struct MissingMediaItem;

// An AI result is applied to a quad only at this confidence or above (ADR-035).
constexpr float AI_MIN_CONFIDENCE = 0.35f;

// A Pick Surface proposal on the AI photo (ADR-053 §2), normalized photo coordinates
// with a top-left origin. Nothing changes until the user applies it.
struct AiSurfaceProposal {
    std::array<std::array<float, 2>, 4> corners{};  // TL, TR, BR, BL
    std::vector<std::array<float, 2>> outline;
    float confidence = 0.0f;
    std::optional<float> planarity;  // nullopt: the sidecar has no depth model
    bool stub = false;               // a placeholder: the sidecar has no SAM 2 model

    bool canApply() const { return !stub && confidence >= AI_MIN_CONFIDENCE; }
};

// The AI Photo panel's state, owned by the application.
struct AiPhoto {
    std::string path;  // the PNG, empty before the first photo
    int serial = 0;    // bumped on each new photo, so a viewer reloads the same path
    std::optional<AiSurfaceProposal> proposal;
};

// What the editor's panels ask of the application (ADR-051 §1): the project session,
// the preferences file and the AI assist. A pure interface: Application implements it,
// the panels reach it through EditorContext::services, and a test passes null or a fake.
class EditorServices {
public:
    virtual ~EditorServices() = default;

    // ---- Project session (ADR-046) -----------------------------------------------
    // The Project tab's "(modified)" label, now.
    virtual bool unsavedLabelShown() = 0;

    // ---- Missing media (PRD §4.1.8) ----------------------------------------------
    virtual bool isSearchingMedia() const = 0;
    virtual bool isFileDialogBusy() const = 0;
    virtual std::string mediaSearchFolder() const = 0;
    virtual void requestLocateMissingMedia(const MissingMediaItem& item) = 0;
    virtual void requestMissingMediaSearch() = 0;

    // ---- Preferences ---------------------------------------------------------------
    // Write the machine preferences (EditorContext::prefs) to their file.
    virtual void savePreferences() = 0;

    // ---- AI pre-show assist (ADR-035) ----------------------------------------------
    // Every action no-ops gracefully without a sidecar. TakePhoto, ApplyProposal and
    // ClearProposal are the AI Photo panel's (ADR-053 §2); ScanOutput and CancelScan
    // the Gray-code scan's (ADR-053 §3).
    enum class AiAction {
        Enable, DetectSurface, EmitPattern, CapturePhoto, ClearPattern, SolveApply, Anamorphic,
        TakePhoto, ApplyProposal, ClearProposal, ScanOutput, CancelScan
    };
    virtual bool aiActive() const = 0;
    // A camera capture or sidecar request is running: the items that start one wait.
    virtual bool aiBusy() const = 0;
    virtual void runAi(AiAction action) = 0;

    // ---- AI Photo panel (ADR-053 §2) -----------------------------------------------
    // The sidecar advertises "segment" (Pick Surface). Cached: no I/O.
    virtual bool aiCanSegment() const = 0;
    // The last AI photo and the proposal on it.
    virtual const AiPhoto& aiPhoto() const = 0;
    // Ask for the surface under a click on the photo (normalized, top-left origin).
    virtual void aiSegmentAt(float x, float y) = 0;

    // ---- Scan Output (ADR-053 §3) --------------------------------------------------
    // The sidecar advertises "graycode". Cached: no I/O.
    virtual bool aiCanScan() const = 0;
    // A scan is showing its patterns or being decoded.
    virtual bool aiScanning() const = 0;
    // This session holds a current Gray-code map of that output (its resolution and
    // display unchanged since the scan).
    virtual bool aiHasScan(const UUID& outputId) const = 0;
    // Assisted calibration: place the selected calibration point's pixel where a click
    // on the AI photo (normalized, top-left origin) maps through the scan.
    virtual void aiCalibrationPixelAt(float x, float y) = 0;
};

}  // namespace quadmapper
