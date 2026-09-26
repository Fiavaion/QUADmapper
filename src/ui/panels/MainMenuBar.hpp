#pragma once

#include <functional>
#include <string>
#include <vector>

namespace quadmapper {

struct EditorContext;
class AudioEngine;

// Renders the application main menu bar (File / Edit / Window).
// Does NOT inherit from Panel — it is always shown and manages its own
// ImGui::BeginMainMenuBar / EndMainMenuBar lifetime.
// File items (New / Open / Save / Save As / Exit) set ctx.es.pendingFileAction,
// which Application runs at the next frame boundary (ADR-046). All other items
// (Edit / Window) operate directly on ctx.es fields.
class MainMenuBar {
public:
    using ProjectCallback = std::function<void()>;

    // AI pre-show assist (ADR-035 / QM-056). The menu is always shown; its items
    // enable/disable purely from aiActiveQuery_, so the optional sidecar's absence
    // degrades cleanly. The query reads cached client state (no per-frame I/O); the
    // two actions launch the sidecar and request a surface. All optional.
    using BoolQuery = std::function<bool()>;
    void setAiActiveQuery(BoolQuery cb)        { aiActiveQuery_   = std::move(cb); }
    void setOnEnableAi(ProjectCallback cb)     { onEnableAi_      = std::move(cb); }
    void setOnDetectSurface(ProjectCallback cb){ onDetectSurface_ = std::move(cb); }

    // One-Flash calibration flow (QM-059): emit a projector pattern, capture a
    // photo, clear the pattern.
    void setOnEmitPattern(ProjectCallback cb)  { onEmitPattern_  = std::move(cb); }
    void setOnCapturePhoto(ProjectCallback cb) { onCapturePhoto_ = std::move(cb); }
    void setOnClearPattern(ProjectCallback cb) { onClearPattern_ = std::move(cb); }
    void setOnSolveApply(ProjectCallback cb)   { onSolveApply_   = std::move(cb); }
    void setOnAnamorphic(ProjectCallback cb)   { onAnamorphic_   = std::move(cb); }

    // Call once per frame inside the ImGui frame, after the dockspace.
    void render(EditorContext& ctx);

private:
    // Master sound (ADR-050 §2): mute, volume and the output device, beside the master
    // timeline so the show's transport and sound live in one strip.
    void renderAudioStrip(EditorContext& ctx, AudioEngine& audio);
    // Output-device choices; `menu` renders menu items, otherwise combo entries.
    // `listOpen` is that list's was-open-last-frame flag: the devices are enumerated
    // once when it opens, not every frame.
    void renderAudioDevices(AudioEngine& audio, bool menu, bool& listOpen);

    std::vector<std::string> audioDevices_;
    bool audioComboOpen_ = false;  // the strip's device combo
    bool audioMenuOpen_ = false;   // Output > Audio Output

    BoolQuery       aiActiveQuery_;
    ProjectCallback onEnableAi_;
    ProjectCallback onDetectSurface_;
    ProjectCallback onEmitPattern_;
    ProjectCallback onCapturePhoto_;
    ProjectCallback onClearPattern_;
    ProjectCallback onSolveApply_;
    ProjectCallback onAnamorphic_;
};

}  // namespace quadmapper
