#include "ProceduralEditorPanel.hpp"
#include "source/SourceManager.hpp"
#include "source/procedural/ProceduralSource.hpp"
#include "media/MediaLibrary.hpp"
#include "media/MediaItem.hpp"
#include <imgui.h>

namespace quadmapper {

ProceduralEditorPanel::ProceduralEditorPanel() : Panel("Procedural Editor") {
    visible_ = false;  // Hidden by default until a source is selected
}

void ProceduralEditorPanel::setEditingSource(const UUID& mediaItemId) {
    editingMediaItemId_ = mediaItemId;
    visible_ = true;

    // Initialize resolution from source
    if (mediaLibrary_ && sourceManager_) {
        MediaItem* item = mediaLibrary_->getItem(mediaItemId);
        if (item && item->hasSource()) {
            auto* source = sourceManager_->getSourceAs<ProceduralSource>(item->getSourceId());
            if (source) {
                editWidth_ = source->getWidth();
                editHeight_ = source->getHeight();
            }
        }
    }
}

void ProceduralEditorPanel::clearEditingSource() {
    editingMediaItemId_ = UUID::null();
    visible_ = false;
}

void ProceduralEditorPanel::render() {
    if (!visible_) return;

    ImGui::SetNextWindowSize(ImVec2(350, 400), ImGuiCond_FirstUseEver);

    bool windowOpen = true;
    if (ImGui::Begin(title_.c_str(), &windowOpen)) {
        if (!sourceManager_ || !mediaLibrary_) {
            ImGui::TextDisabled("Editor not connected to source manager");
            ImGui::End();
            return;
        }

        if (editingMediaItemId_.isNull()) {
            ImGui::TextDisabled("No procedural source selected");
            ImGui::TextDisabled("Select a procedural item in the Media Bin");
            ImGui::End();
            return;
        }

        // Get the media item
        MediaItem* item = mediaLibrary_->getItem(editingMediaItemId_);
        if (!item) {
            ImGui::TextDisabled("Media item not found");
            ImGui::End();
            return;
        }

        // Get the procedural source
        if (!item->hasSource()) {
            ImGui::TextDisabled("No source linked to this item");
            ImGui::End();
            return;
        }

        auto* source = sourceManager_->getSourceAs<ProceduralSource>(item->getSourceId());
        if (!source) {
            ImGui::TextDisabled("Source is not a procedural type");
            ImGui::End();
            return;
        }

        // Header
        ImGui::Text("Editing: %s", item->getName().c_str());
        ImGui::TextDisabled("Type: %s", source->getName().c_str());
        ImGui::Separator();

        // Animation indicator
        if (source->isAnimated()) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "[ANIMATED]");
        }

        // Resolution controls
        if (ImGui::CollapsingHeader("Resolution", ImGuiTreeNodeFlags_DefaultOpen)) {
            renderResolutionControls(source);
        }

        // Parameter controls
        if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
            renderParameterControls(source);
        }

        // Actions
        ImGui::Separator();
        if (ImGui::Button("Reset to Defaults")) {
            source->resetParameters();
        }
    }
    ImGui::End();

    if (!windowOpen) {
        clearEditingSource();
    }
}

void ProceduralEditorPanel::renderResolutionControls(ProceduralSource* source) {
    ImGui::Text("Current: %d x %d", source->getWidth(), source->getHeight());

    // Width/Height inputs
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("Width", &editWidth_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("Height", &editHeight_);

    // Clamp values
    editWidth_ = std::max(64, std::min(8192, editWidth_));
    editHeight_ = std::max(64, std::min(8192, editHeight_));

    // Apply button
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        source->setResolution(editWidth_, editHeight_);
    }

    // Presets dropdown
    if (ImGui::Button("Presets...")) {
        showPresets_ = !showPresets_;
    }

    if (showPresets_) {
        ImGui::Indent();
        for (int i = 0; i < PRESET_COUNT; ++i) {
            if (ImGui::Selectable(PRESET_NAMES[i])) {
                editWidth_ = PRESET_WIDTHS[i];
                editHeight_ = PRESET_HEIGHTS[i];
                source->setResolution(editWidth_, editHeight_);
                showPresets_ = false;
            }
        }
        ImGui::Unindent();
    }
}

void ProceduralEditorPanel::renderParameterControls(ProceduralSource* source) {
    auto params = source->getParameters();

    if (params.empty()) {
        ImGui::TextDisabled("No adjustable parameters");
        return;
    }

    for (const auto& param : params) {
        ImGui::PushID(param.name.c_str());

        switch (param.type) {
            case SourceParameter::Type::Float: {
                float value = std::get<float>(param.value);
                if (ImGui::SliderFloat(param.displayName.c_str(), &value,
                                       param.minValue, param.maxValue)) {
                    source->setParameter(param.name, value);
                }
                break;
            }

            case SourceParameter::Type::Int: {
                int value = std::get<int>(param.value);
                if (ImGui::SliderInt(param.displayName.c_str(), &value,
                                     static_cast<int>(param.minValue),
                                     static_cast<int>(param.maxValue))) {
                    source->setParameter(param.name, value);
                }
                break;
            }

            case SourceParameter::Type::Bool: {
                bool value = std::get<bool>(param.value);
                if (ImGui::Checkbox(param.displayName.c_str(), &value)) {
                    source->setParameter(param.name, value);
                }
                break;
            }

            case SourceParameter::Type::Color: {
                Color color = std::get<Color>(param.value);
                float col[4] = { color.r, color.g, color.b, color.a };
                if (ImGui::ColorEdit4(param.displayName.c_str(), col,
                                      ImGuiColorEditFlags_AlphaBar)) {
                    source->setParameter(param.name, Color{col[0], col[1], col[2], col[3]});
                }
                break;
            }
        }

        ImGui::PopID();
    }
}

// Static member definitions
constexpr int ProceduralEditorPanel::PRESET_WIDTHS[PRESET_COUNT];
constexpr int ProceduralEditorPanel::PRESET_HEIGHTS[PRESET_COUNT];
constexpr const char* ProceduralEditorPanel::PRESET_NAMES[PRESET_COUNT];

} // namespace quadmapper
