#include "PanelHost.hpp"

#include "core/Logger.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"

#include <imgui.h>

#include <algorithm>

namespace quadmapper {

void PanelHost::add(Panel& panel) {
    if (find(panel.id())) {
        Logger::error("PanelHost: a panel '{}' is already registered", panel.id());
        return;
    }
    panels_.push_back(&panel);
    defaultVisible_.push_back(panel.visible);
}

void PanelHost::attach(EditorContext& ctx) {
    for (Panel* panel : panels_) panel->onAttach(ctx);
}

void PanelHost::projectReplaced(EditorContext& ctx) {
    for (Panel* panel : panels_) panel->onProjectReplaced(ctx);
}

Panel* PanelHost::find(const std::string& id) const {
    for (Panel* panel : panels_) {
        if (id == panel->id()) return panel;
    }
    return nullptr;
}

bool PanelHost::isVisible(const std::string& id) const {
    const Panel* panel = find(id);
    return panel && panel->visible;
}

void PanelHost::drainOpenRequests(EditorState& es) {
    for (const std::string& id : es.panelOpenRequests) {
        if (Panel* panel = find(id)) {
            panel->visible = true;
            focusNext_.push_back(panel);
        } else {
            Logger::warning("PanelHost: no panel '{}' to open", id);
        }
    }
    es.panelOpenRequests.clear();
}

bool PanelHost::drawDisabled(const Panel& panel, const EditorContext& ctx) const {
    return ctx.es.performLock.isOn() && panel.editsShow();
}

void PanelHost::drawWindows(EditorContext& ctx) {
    drainOpenRequests(ctx.es);
    for (Panel* panel : panels_) {
        const PanelKind kind = panel->kind();
        if (kind == PanelKind::Overlay) continue;
        panel->tick(ctx);
        if (kind != PanelKind::Window || !panel->visible || !panel->available(ctx)) continue;

        panel->beforeBegin(ctx);
        if (std::find(focusNext_.begin(), focusNext_.end(), panel) != focusNext_.end()) {
            ImGui::SetNextWindowFocus();
        }
        const bool disabled = drawDisabled(*panel, ctx);
        if (ImGui::Begin(panel->id(), &panel->visible, panel->windowFlags())) {
            if (disabled) ImGui::BeginDisabled();
            panel->drawContent(ctx);
            if (disabled) ImGui::EndDisabled();
        }
        ImGui::End();
    }
    focusNext_.clear();
}

void PanelHost::drawTabs(EditorContext& ctx) {
    for (Panel* panel : panels_) {
        if (panel->kind() != PanelKind::Tab || !panel->available(ctx)) continue;
        if (!ImGui::BeginTabItem(panel->id())) continue;
        const bool disabled = drawDisabled(*panel, ctx);
        if (disabled) ImGui::BeginDisabled();
        panel->drawContent(ctx);
        if (disabled) ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
}

void PanelHost::drawOverlays(EditorContext& ctx) {
    for (Panel* panel : panels_) {
        if (panel->kind() != PanelKind::Overlay) continue;
        panel->tick(ctx);
        if (!panel->available(ctx)) continue;
        const bool disabled = drawDisabled(*panel, ctx);
        if (disabled) ImGui::BeginDisabled();
        panel->drawContent(ctx);
        if (disabled) ImGui::EndDisabled();
    }
}

void PanelHost::drawWindowMenuItems() {
    for (Panel* panel : panels_) {
        if (panel->kind() != PanelKind::Window || !panel->menuLabel()) continue;
        ImGui::MenuItem(panel->menuLabel(), nullptr, &panel->visible);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Reset Layout")) {
        for (size_t i = 0; i < panels_.size(); ++i) {
            Panel& panel = *panels_[i];
            if (panel.kind() == PanelKind::Window && panel.menuLabel() && defaultVisible_[i]) {
                panel.visible = true;
            }
        }
        layoutReset_ = true;
    }
}

bool PanelHost::takeLayoutReset() {
    const bool reset = layoutReset_;
    layoutReset_ = false;
    return reset;
}

}  // namespace quadmapper
