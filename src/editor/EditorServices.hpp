#pragma once

#include <string>

namespace quadmapper {

struct MissingMediaItem;

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
    // Every action no-ops gracefully without a sidecar.
    enum class AiAction { Enable, DetectSurface, EmitPattern, CapturePhoto, ClearPattern, SolveApply, Anamorphic };
    virtual bool aiActive() const = 0;
    // A camera capture or sidecar request is running: the items that start one wait.
    virtual bool aiBusy() const = 0;
    virtual void runAi(AiAction action) = 0;
};

}  // namespace quadmapper
