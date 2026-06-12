#pragma once

#include "Panel.hpp"
#include "core/UUID.hpp"
#include <string>

namespace quadmapper {

class OutputManager;
class OutputWindow;

/**
 * OutputPanel provides UI for managing output windows.
 * Users can add/remove outputs, configure resolution/monitor settings,
 * and enter/exit perform mode.
 */
class OutputPanel : public Panel {
public:
    OutputPanel();

    /**
     * Set the output manager.
     */
    void setOutputManager(OutputManager* manager) { outputManager_ = manager; }

    /**
     * Render the panel.
     */
    void render() override;

    /**
     * Render just the panel content (without window wrapper).
     * Useful for embedding in tabs.
     */
    void renderContent();

    /**
     * Get the currently selected output ID.
     */
    const UUID& getSelectedOutputId() const { return selectedOutputId_; }

    /**
     * Check if there is a selected output.
     */
    bool hasSelection() const { return !selectedOutputId_.isNull(); }

    /**
     * Clear the current selection.
     */
    void clearSelection() { selectedOutputId_ = UUID::null(); }

private:
    // Render the perform mode section
    void renderPerformModeSection();

    // Render the output list
    void renderOutputList();

    // Render a single output window row
    void renderOutputRow(OutputWindow* output, int index);

    // Render the add/remove buttons
    void renderActionButtons();

    // Render the properties section for selected output
    void renderOutputProperties();

    // Render monitor dropdown
    void renderMonitorDropdown(OutputWindow* output);

    // Render resolution controls
    void renderResolutionControls(OutputWindow* output);

    // Render rotation controls
    void renderRotationControls(OutputWindow* output);

    OutputManager* outputManager_ = nullptr;
    UUID selectedOutputId_;

    // State for renaming
    bool isRenaming_ = false;
    int renamingIndex_ = -1;
    char renameBuffer_[128] = "";
};

} // namespace quadmapper
