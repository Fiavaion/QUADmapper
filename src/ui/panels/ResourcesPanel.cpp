#include "ResourcesPanel.hpp"

#include "editor/EditorContext.hpp"
#include "editor/EditorServices.hpp"
#include "editor/EditorState.hpp"
#include "project/Project.hpp"
#include "ui/PanelHost.hpp"

#include <imgui.h>

namespace quadmapper {

void ResourcesPanel::drawContent(EditorContext& ctx) {
    if (ImGui::BeginTabBar("ResourcesTabs")) {
        host_.drawTabs(ctx);
        ImGui::EndTabBar();
    }
}

bool ProjectTab::available(const EditorContext& ctx) const {
    return ctx.es.uiMode == EditorState::UiMode::Advanced;
}

void ProjectTab::drawContent(EditorContext& ctx) {
    EditorState& es = ctx.es;
    ImGui::Text("Project: %s", ctx.project ? ctx.project->getName().c_str() : "");
    if (ctx.services && ctx.services->unsavedLabelShown()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "(modified)");
    }
    ImGui::Separator();

    // Save and Load only queue the file action; it runs at the next frame boundary
    // (ADR-046), as the File menu's do.
    ImGui::Text("File Path:");
    ImGui::InputText("##projectpath", es.projectPath, sizeof(es.projectPath));
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        es.pendingFileAction = EditorState::FileAction::Save;
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        es.pendingLoadPath = es.projectPath;
        es.pendingFileAction = EditorState::FileAction::Load;
    }

    // Status message display
    if (es.statusTimer > 0.0f) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", es.statusMessage);
        es.statusTimer -= ImGui::GetIO().DeltaTime;
    }
}

}  // namespace quadmapper
