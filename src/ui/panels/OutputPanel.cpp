#include "OutputPanel.hpp"
#include "ui/ConfirmPopup.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include <imgui.h>
#include <cstring>

namespace quadmapper {

bool OutputPanel::hasSelection() const { return !es_->selectedOutputId.isNull(); }

bool OutputPanel::available(const EditorContext& ctx) const {
    return ctx.es.uiMode == EditorState::UiMode::Advanced;
}

void OutputPanel::onAttach(EditorContext& ctx) {
    es_ = &ctx.es;
    outputManager_ = ctx.outputManager;
}

void OutputPanel::drawContent(EditorContext& ctx) {
    EditorState& es = ctx.es;
    const UUID prevOutputSelection = es.selectedOutputId;
    renderContent();
    // Switch to Output properties if an output was just selected
    if (!es.selectedOutputId.isNull() && es.selectedOutputId != prevOutputSelection) {
        es.currentSelectionType = EditorState::SelectionType::Output;
        es.quadManager().deselectAll();
        es.selectedMediaId = UUID();
    }
}

void OutputPanel::renderContent() {
    if (!outputManager_ || !es_) {
        ImGui::TextDisabled("No output manager connected");
        return;
    }

    renderPerformModeSection();
    ImGui::Separator();
    renderActionButtons();
    ImGui::Separator();
    renderOutputList();
    renderDeleteConfirm();

    // Show properties for selected output
    if (hasSelection()) {
        ImGui::Separator();
        renderOutputProperties();
    }
}

void OutputPanel::renderPerformModeSection() {
    bool inPerformMode = outputManager_->isInPerformMode();

    // Perform mode toggle button
    if (inPerformMode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));

        if (ImGui::Button("Exit Perform Mode (F11)", ImVec2(-1, 30))) {
            outputManager_->exitPerformMode();
        }

        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));

        if (ImGui::Button("Enter Perform Mode (F11)", ImVec2(-1, 30))) {
            outputManager_->enterPerformMode();
        }

        ImGui::PopStyleColor(3);
    }

    if (ImGui::IsItemHovered()) {
        if (inPerformMode) {
            ImGui::SetTooltip("Exit perform mode and return to edit mode\nPress F11 or Escape to exit");
        } else {
            ImGui::SetTooltip("Enter perform mode to display outputs fullscreen\nAll enabled outputs will show on their assigned monitors");
        }
    }

    // Status display
    if (inPerformMode) {
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "PERFORM MODE ACTIVE");
    }
}

void OutputPanel::renderActionButtons() {
    // Add output button
    if (ImGui::Button("Add Output")) {
        OutputWindow* newOutput = outputManager_->createOutput();
        if (newOutput) {
            es_->selectedOutputId = newOutput->getId();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create a new output window");
    }

    ImGui::SameLine();

    // Remove output button
    ImGui::BeginDisabled(!hasSelection() || outputManager_->getOutputCount() <= 1);
    if (ImGui::Button("Remove Output")) {
        if (hasSelection()) requestDelete(es_->selectedOutputId);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (outputManager_->getOutputCount() <= 1) {
            ImGui::SetTooltip("Cannot remove the last output");
        } else if (!hasSelection()) {
            ImGui::SetTooltip("Select an output to remove");
        } else {
            ImGui::SetTooltip("Remove the selected output");
        }
    }

    ImGui::SameLine();

    // Refresh monitors button
    if (ImGui::Button("Refresh Monitors")) {
        outputManager_->refreshMonitors();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Detect connected monitors");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(%zu outputs, %zu monitors)",
        outputManager_->getOutputCount(),
        outputManager_->getMonitorCount());
}

void OutputPanel::renderOutputList() {
    ImGui::Text("Outputs:");
    ImGui::BeginChild("OutputList", ImVec2(0, 150), true);

    const auto& outputs = outputManager_->getOutputs();
    for (size_t i = 0; i < outputs.size(); ++i) {
        renderOutputRow(outputs[i].get(), static_cast<int>(i));
    }

    ImGui::EndChild();

    // Asked for from a row's menu: the confirmation opens once the list loop is done.
    if (!menuDeleteId_.isNull()) {
        requestDelete(menuDeleteId_);
        menuDeleteId_ = UUID::null();
    }
}

void OutputPanel::requestDelete(const UUID& id) {
    const OutputWindow* output = outputManager_->getOutput(id);
    if (!output || outputManager_->getOutputCount() <= 1) return;
    pendingDeleteId_ = id;
    deleteConfirm_.request(
        "Remove Output?",
        "Remove output '" + output->getName() +
            "'? Its routing, slices and calibration are removed. This cannot be undone.",
        "Remove");
}

void OutputPanel::renderDeleteConfirm() {
    if (!deleteConfirm_.draw()) return;
    const UUID id = pendingDeleteId_;
    pendingDeleteId_ = UUID::null();
    if (!outputManager_->getOutput(id) || outputManager_->getOutputCount() <= 1) return;
    outputManager_->deleteOutput(id);
    if (es_->selectedOutputId == id) {
        es_->selectedOutputId = UUID::null();
        const auto& outputs = outputManager_->getOutputs();
        if (!outputs.empty()) es_->selectedOutputId = outputs[0]->getId();
    }
}

void OutputPanel::renderOutputRow(OutputWindow* output, int index) {
    ImGui::PushID(index);

    bool isSelected = (output->getId() == es_->selectedOutputId);

    // Enabled checkbox
    bool enabled = output->isEnabled();
    if (ImGui::Checkbox("##enabled", &enabled)) {
        output->setEnabled(enabled);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enable/disable this output");
    }

    ImGui::SameLine();

    // Output name (selectable, double-click to rename)
    if (isRenaming_ && renamingIndex_ == index) {
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##rename", renameBuffer_, sizeof(renameBuffer_),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            if (!isBlankName(renameBuffer_)) output->setName(renameBuffer_);
            isRenaming_ = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0))) {
            isRenaming_ = false;
        }
    } else {
        // Color based on enabled state
        if (!enabled) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        }

        ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick;
        if (ImGui::Selectable(output->getName().c_str(), isSelected, flags)) {
            es_->selectedOutputId = output->getId();

            if (ImGui::IsMouseDoubleClicked(0)) {
                isRenaming_ = true;
                renamingIndex_ = index;
                std::strncpy(renameBuffer_, output->getName().c_str(), sizeof(renameBuffer_) - 1);
                renameBuffer_[sizeof(renameBuffer_) - 1] = '\0';
            }
        }

        if (!enabled) {
            ImGui::PopStyleColor();
        }
    }

    // Show resolution and monitor info on the same line
    ImGui::SameLine(200);
    IVec2 res = output->getResolution();
    const MonitorInfo* info = outputManager_->getMonitor(output->getMonitor());
    ImGui::TextDisabled("%dx%d | %s", res.x, res.y,
                        info ? info->name.c_str() : "Not connected");

    // Context menu - use explicit ID to avoid issues with TextDisabled having no item ID
    char contextMenuId[32];
    snprintf(contextMenuId, sizeof(contextMenuId), "output_ctx_%d", index);
    if (ImGui::BeginPopupContextItem(contextMenuId)) {
        ImGui::TextDisabled("%s", output->getName().c_str());
        ImGui::Separator();

        if (ImGui::MenuItem("Rename", "Double-click")) {
            isRenaming_ = true;
            renamingIndex_ = index;
            std::strncpy(renameBuffer_, output->getName().c_str(), sizeof(renameBuffer_) - 1);
            renameBuffer_[sizeof(renameBuffer_) - 1] = '\0';
        }

        bool canDelete = outputManager_->getOutputCount() > 1;
        ImGui::BeginDisabled(!canDelete);
        if (ImGui::MenuItem("Delete")) menuDeleteId_ = output->getId();
        ImGui::EndDisabled();

        ImGui::EndPopup();
    }

    ImGui::PopID();
}

void OutputPanel::renderOutputProperties() {
    OutputWindow* output = outputManager_->getOutput(es_->selectedOutputId);
    if (!output) {
        es_->selectedOutputId = UUID::null();
        return;
    }

    ImGui::Text("Output Properties: %s", output->getName().c_str());

    // Monitor assignment
    renderMonitorDropdown(output);

    // Resolution
    renderResolutionControls(output);

    // Rotation
    renderRotationControls(output);
}

void OutputPanel::renderMonitorDropdown(OutputWindow* output) {
    ImGui::Text("Monitor:");
    ImGui::SameLine(100);

    int currentMonitor = output->getMonitor();
    const auto& monitors = outputManager_->getMonitors();
    // No chosen display = automatic: the first one that is not the editor's (QM-090).
    const bool automatic = output->isMonitorAutomatic();
    const std::string previewStr = OutputManager::describeMonitorChoice(monitors, *output);

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##monitor", previewStr.c_str())) {
        if (ImGui::Selectable("Automatic (not the editor's display)", automatic)) {
            outputManager_->assignMonitor(*output, -1);
        }

        ImGui::Separator();

        // Available monitors
        for (size_t i = 0; i < monitors.size(); i++) {
            bool isSelected = !automatic && static_cast<int>(i) == currentMonitor;
            std::string label = monitors[i].getDisplayString();
            if (monitors[i].isPrimary) {
                label += " (Primary)";
            }

            if (ImGui::Selectable(label.c_str(), isSelected)) {
                outputManager_->assignMonitor(*output, static_cast<int>(i));
            }
        }

        ImGui::EndCombo();
    }
}

void OutputPanel::renderResolutionControls(OutputWindow* output) {
    ImGui::Text("Resolution:");
    ImGui::SameLine(100);

    IVec2 res = output->getResolution();

    // Preset dropdown
    ImGui::SetNextItemWidth(150);
    std::string presetLabel = std::to_string(res.x) + "x" + std::to_string(res.y);

    // Check if matches a preset
    int matchedPreset = -1;
    for (size_t i = 0; i < RESOLUTION_PRESET_COUNT - 1; i++) {  // Exclude "Custom"
        if (RESOLUTION_PRESETS[i].width == res.x && RESOLUTION_PRESETS[i].height == res.y) {
            matchedPreset = static_cast<int>(i);
            presetLabel = RESOLUTION_PRESETS[i].name;
            break;
        }
    }
    if (matchedPreset < 0) {
        presetLabel = "Custom (" + std::to_string(res.x) + "x" + std::to_string(res.y) + ")";
    }

    if (ImGui::BeginCombo("##resolution", presetLabel.c_str())) {
        for (size_t i = 0; i < RESOLUTION_PRESET_COUNT - 1; i++) {
            bool isSelected = (static_cast<int>(i) == matchedPreset);
            std::string label = std::string(RESOLUTION_PRESETS[i].name) + " (" +
                               std::to_string(RESOLUTION_PRESETS[i].width) + "x" +
                               std::to_string(RESOLUTION_PRESETS[i].height) + ")";

            if (ImGui::Selectable(label.c_str(), isSelected)) {
                output->setResolution(IVec2(RESOLUTION_PRESETS[i].width, RESOLUTION_PRESETS[i].height));
            }
        }
        ImGui::EndCombo();
    }

    // Custom resolution inputs
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    int width = res.x;
    if (ImGui::InputInt("##width", &width, 0, 0)) {
        if (width > 0 && width <= 7680) {  // Max 8K
            output->setResolution(IVec2(width, res.y));
        }
    }

    ImGui::SameLine();
    ImGui::Text("x");
    ImGui::SameLine();

    ImGui::SetNextItemWidth(60);
    int height = res.y;
    if (ImGui::InputInt("##height", &height, 0, 0)) {
        if (height > 0 && height <= 4320) {  // Max 8K
            output->setResolution(IVec2(res.x, height));
        }
    }
}

void OutputPanel::renderRotationControls(OutputWindow* output) {
    ImGui::Text("Rotation:");
    ImGui::SameLine(100);

    int degrees = output->getRotationDegrees();

    ImGui::SetNextItemWidth(100);
    const char* rotationOptions[] = { "0", "90", "180", "270" };
    int currentIdx = degrees / 90;
    if (currentIdx < 0 || currentIdx > 3) currentIdx = 0;

    if (ImGui::BeginCombo("##rotation", rotationOptions[currentIdx])) {
        for (int i = 0; i < 4; i++) {
            bool isSelected = (i == currentIdx);
            if (ImGui::Selectable(rotationOptions[i], isSelected)) {
                output->setRotationDegrees(i * 90);
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Rotate the output content clockwise (projector mounted on its side)");
    }

    // Show effective resolution after rotation
    IVec2 effectiveRes = output->getEffectiveResolution();
    ImGui::SameLine();
    ImGui::TextDisabled("(Effective: %dx%d)", effectiveRes.x, effectiveRes.y);
}

} // namespace quadmapper
