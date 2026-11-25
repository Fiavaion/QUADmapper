#include "OutputPreviewPanel.hpp"

#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "scene/Scene.hpp"

#include <imgui.h>

namespace quadmapper {

bool OutputPreviewPanel::available(const EditorContext& ctx) const {
    return ctx.es.uiMode == EditorState::UiMode::Advanced;
}

void OutputPreviewPanel::drawContent(EditorContext& ctx) {
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

    // Thumbnail display width: fit the panel, capped; the height follows each
    // output's own frame aspect.
    float thumbW = ImGui::GetContentRegionAvail().x - 8.0f;
    if (thumbW > 360.0f) thumbW = 360.0f;
    if (thumbW < 80.0f) thumbW = 80.0f;

    int i = 0;
    for (const auto& output : outputs) {
        if (!output) continue;
        ImGui::PushID(i++);

        const Scene* scene = OutputManager::sceneForOutput(ctx.es.scenes, *output);
        ImGui::Text("%s", output->getName().c_str());
        ImGui::SameLine();
        if (scene) {
            ImGui::TextDisabled("-> %s", scene->getName().c_str());
        } else {
            ImGui::TextDisabled("-> (no scene)");
        }

        // The very texture the projector window shows (ADR-054).
        const Framebuffer* frame = om->outputFrame(output->getId());
        const float aspect = frame ? static_cast<float>(frame->height()) /
                                         static_cast<float>(frame->width())
                                   : 9.0f / 16.0f;
        const ImVec2 thumbSize(thumbW, thumbW * aspect);
        if (frame) {
            // The frame is bottom-up, so flip V (uv0={0,1}, uv1={1,0}).
            ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(frame->colorTexture())),
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
