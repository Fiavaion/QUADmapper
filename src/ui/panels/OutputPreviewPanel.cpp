#include "OutputPreviewPanel.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "scene/Scene.hpp"

#include <imgui.h>

namespace quadmapper {

void OutputPreviewPanel::renderContent(EditorContext& ctx) {
    OutputManager* om = ctx.outputManager;
    if (!om) {
        ImGui::TextDisabled("No output manager.");
        return;
    }

    const auto& outputs = om->getOutputs();
    if (outputs.empty()) {
        ImGui::TextDisabled("No outputs. Add one in the Outputs tab.");
        return;
    }

    // Thumbnail display width: fit the panel, capped, preserving the render aspect.
    const float aspect = static_cast<float>(om->previewHeight()) /
                         static_cast<float>(om->previewWidth());
    float thumbW = ImGui::GetContentRegionAvail().x - 8.0f;
    if (thumbW > 360.0f) thumbW = 360.0f;
    if (thumbW < 80.0f) thumbW = 80.0f;
    const ImVec2 thumbSize(thumbW, thumbW * aspect);

    int i = 0;
    for (const auto& output : outputs) {
        if (!output) continue;
        ImGui::PushID(i++);

        const Scene* scene = OutputManager::sceneForOutput(ctx.es.scenes, output->getId());
        ImGui::Text("%s", output->getName().c_str());
        ImGui::SameLine();
        if (scene) {
            ImGui::TextDisabled("-> %s", scene->getName().c_str());
        } else {
            ImGui::TextDisabled("-> (no scene)");
        }

        unsigned int tex = om->previewTextureFor(output->getId());
        if (tex != 0) {
            // FBO color texture is bottom-up, so flip V (uv0={0,1}, uv1={1,0}).
            ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(tex)),
                         thumbSize, ImVec2(0, 1), ImVec2(1, 0));
        } else {
            ImGui::Dummy(thumbSize);
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::TextDisabled("Live preview of each output's assigned scene.");
}

}  // namespace quadmapper
