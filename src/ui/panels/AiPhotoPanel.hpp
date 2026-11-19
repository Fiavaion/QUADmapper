#pragma once

#include "Panel.hpp"

#include <memory>

namespace quadmapper {

struct EditorContext;
class Texture;

// Pick Surface (ADR-053 §2): the last AI camera photo, where a click asks the sidecar
// for the surface under it. The proposal (the mask outline, its four corners and a
// confidence and planarity readout) is drawn over the photo, and nothing changes until
// Apply warps the selected quad onto it. Opened from AI > Pick Surface...
// While the Calibration panel's Place From Photo waits (ADR-053 §3), a click places the
// selected calibration point instead, through the output's Gray-code scan.
//
// No I/O of its own beyond showing the photo: the camera, the sidecar and the warp go
// through EditorServices. Disabled under the perform lock (editsShow).
class AiPhotoPanel : public Panel {
public:
    static constexpr const char* ID = "AI Photo";

    AiPhotoPanel();
    ~AiPhotoPanel() override;
    AiPhotoPanel(const AiPhotoPanel&) = delete;
    AiPhotoPanel& operator=(const AiPhotoPanel&) = delete;

    const char* id() const override { return ID; }
    // Opened from AI > Pick Surface... and Calibration's Place From Photo, not the Window
    // menu; drawn only while AI Assist (a beta, Preferences > General) is on.
    const char* menuLabel() const override { return nullptr; }
    bool available(const EditorContext& ctx) const override;
    void drawContent(EditorContext& ctx) override;

private:
    // The photo as a texture, reloaded when the services report a new one. Freed with
    // the panel, which goes while the editor's GL context is current.
    std::unique_ptr<Texture> photo_;
    int loadedSerial_ = 0;
};

}  // namespace quadmapper
