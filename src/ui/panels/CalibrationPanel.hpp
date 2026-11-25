#pragma once

#include "Panel.hpp"
#include "core/UUID.hpp"

#include <memory>

namespace quadmapper {

struct EditorContext;
class Framebuffer;

// Projector calibration (ADR-052 §6), shown while EditorState::calibratingOutputId is
// set: the lens, the point table, the solve status, and a model view that renders the
// output's scene objects from an orbit camera, where a click picks a point's 3D
// position. The canvas side (placing, dragging and nudging each point's pixel) is in
// ViewportEditor; the rules live in scene/CalibrationSession.
//
// An Overlay panel: its window exists only while calibrating (leaving calibration closes
// it, and closing it leaves calibration), so it opens and closes that window itself.
class CalibrationPanel : public Panel {
public:
    static constexpr const char* ID = "Calibration";

    CalibrationPanel();
    ~CalibrationPanel() override;
    CalibrationPanel(const CalibrationPanel&) = delete;
    CalibrationPanel& operator=(const CalibrationPanel&) = delete;

    const char* id() const override { return ID; }
    const char* menuLabel() const override { return nullptr; }  // Edit > Calibrate Projector...
    PanelKind kind() const override { return PanelKind::Overlay; }
    // The window, then the canvas markers (drawCanvasOverlay).
    void drawContent(EditorContext& ctx) override;

    // Once a frame inside the ImGui frame. Also runs the live solve (at most once a
    // frame), after this frame's canvas and panel edits.
    void render(EditorContext& ctx);

    // Once a frame inside the ImGui frame: each point's target pixel on the editor
    // canvas as a numbered marker, and where the projector currently lands its 3D
    // point (a cross, joined to the marker) so the error is visible.
    void drawCanvasOverlay(EditorContext& ctx);

private:
    std::unique_ptr<Framebuffer> modelView_;
    UUID shownOutputId_;   // output shown last frame; a new one takes focus
    float orbitYaw_ = 30.0f;
    float orbitPitch_ = 20.0f;
    float orbitDistance_ = 0.0f;  // <= 0: frame the objects on the next draw
    bool orbitDragged_ = false;   // the current press has orbited, so it is not a click
    bool snapToVertex_ = true;
};

}  // namespace quadmapper
