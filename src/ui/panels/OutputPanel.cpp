#include "OutputPanel.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include <imgui.h>
#include <cstring>

namespace quadmapper {

OutputPanel::OutputPanel() : Panel("Outputs") {
    selectedOutputId_ = UUID::null();
}

void OutputPanel::render() {
    if (!visible_) return;

    ImGui::Begin(title_.c_str(), &visible_);
    renderContent();
    ImGui::End();
}

void OutputPanel::renderContent() {
    if (!outputManager_) {
        ImGui::TextDisabled("No output manager connected");
        return;
    }

    renderPerformModeSection();
    ImGui::Separator();
    renderActionButtons();
    ImGui::Separator();
    renderOutputList();

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
            selectedOutputId_ = newOutput->getId();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create a new output window");
    }

    ImGui::SameLine();

    // Remove output button
    ImGui::BeginDisabled(!hasSelection() || outputManager_->getOutputCount() <= 1);
    if (ImGui::Button("Remove Output")) {
        if (hasSelection()) {
            outputManager_->deleteOutput(selectedOutputId_);
            selectedOutputId_ = UUID::null();

            // Select first remaining output
            const auto& outputs = outputManager_->getOutputs();
            if (!outputs.empty()) {
                selectedOutputId_ = outputs[0]->getId();
            }
        }
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
}

void OutputPanel::renderOutputRow(OutputWindow* output, int index) {
    ImGui::PushID(index);

    bool isSelected = (output->getId() == selectedOutputId_);

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
            output->setName(renameBuffer_);
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
            selectedOutputId_ = output->getId();

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
    int monitorIdx = output->getMonitor();
    const char* monitorName = "No Monitor";
    if (monitorIdx >= 0 && static_cast<size_t>(monitorIdx) < outputManager_->getMonitorCount()) {
        const MonitorInfo* info = outputManager_->getMonitor(monitorIdx);
        if (info) {
            monitorName = info->name.c_str();
        }
    }
    ImGui::TextDisabled("%dx%d | %s", res.x, res.y, monitorName);

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
        if (ImGui::MenuItem("Delete")) {
            outputManager_->deleteOutput(output->getId());
            if (selectedOutputId_ == output->getId()) {
                selectedOutputId_ = UUID::null();
            }
        }
        ImGui::EndDisabled();

        ImGui::EndPopup();
    }

    ImGui::PopID();
}

void OutputPanel::renderOutputProperties() {
    OutputWindow* output = outputManager_->getOutput(selectedOutputId_);
    if (!output) {
        selectedOutputId_ = UUID::null();
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

    // Build preview string
    std::string previewStr = "Not Assigned";
    if (currentMonitor >= 0 && static_cast<size_t>(currentMonitor) < monitors.size()) {
        previewStr = monitors[currentMonitor].getDisplayString();
    }

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##monitor", previewStr.c_str())) {
        // Option for no monitor
        bool isNone = (currentMonitor < 0);
        if (ImGui::Selectable("Not Assigned", isNone)) {
            output->setMonitor(-1);
        }

        ImGui::Separator();

        // Available monitors
        for (size_t i = 0; i < monitors.size(); i++) {
            bool isSelected = (static_cast<int>(i) == currentMonitor);
            std::string label = monitors[i].getDisplayString();
            if (monitors[i].isPrimary) {
                label += " (Primary)";
            }

            if (ImGui::Selectable(label.c_str(), isSelected)) {
                output->setMonitor(static_cast<int>(i));
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
        ImGui::SetTooltip("Rotate the output content");
    }

    // Show effective resolution after rotation
    IVec2 effectiveRes = output->getEffectiveResolution();
    ImGui::SameLine();
    ImGui::TextDisabled("(Effective: %dx%d)", effectiveRes.x, effectiveRes.y);
}

} // namespace quadmapper
