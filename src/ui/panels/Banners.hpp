#pragma once

#include "Panel.hpp"

namespace quadmapper {

// Show-state banners over the canvas, on the foreground draw list: BLACKOUT across the
// top while every projector is black (ADR-048 §3), and BLEND TEST PATTERN across the
// bottom while the projectors show the test pattern (ADR-047 §3). The perform lock's
// LOCKED banner joins them in E7.
class Banners : public Panel {
public:
    static constexpr const char* ID = "Banners";

    const char* id() const override { return ID; }
    const char* menuLabel() const override { return nullptr; }
    PanelKind kind() const override { return PanelKind::Overlay; }
    bool editsShow() const override { return false; }  // nothing to click
    void drawContent(EditorContext& ctx) override;
};

}  // namespace quadmapper
