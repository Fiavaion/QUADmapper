#pragma once

#include "Panel.hpp"
#include "core/UUID.hpp"
#include <string>

namespace quadmapper {

struct EditorState;
class OutputManager;
class OutputWindow;

/**
 * OutputPanel provides UI for managing output windows.
 * Users can add/remove outputs, configure resolution/monitor settings,
 * and enter/exit perform mode.
 */
class OutputPanel : public Panel {
public:
    // The selected output lives in `es` (EditorState::selectedOutputId), which
    // outlives the panel.
    explicit OutputPanel(EditorState& es);

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

private:
    bool hasSelection() const;

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

    EditorState& es_;
    OutputManager* outputManager_ = nullptr;

    // State for renaming
    bool isRenaming_ = false;
    int renamingIndex_ = -1;
    char renameBuffer_[128] = "";
};

} // namespace quadmapper
