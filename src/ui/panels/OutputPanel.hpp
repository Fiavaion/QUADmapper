#pragma once

#include "Panel.hpp"
#include "core/UUID.hpp"
#include <string>

namespace quadmapper {

struct EditorContext;
struct EditorState;
class OutputManager;
class OutputWindow;

/**
 * The Outputs tab of the Resources window (Advanced mode): add/remove outputs,
 * configure resolution/monitor settings, and enter/exit perform mode. The selected
 * output lives in EditorState::selectedOutputId.
 */
class OutputPanel : public Panel {
public:
    static constexpr const char* ID = "Outputs";

    const char* id() const override { return ID; }
    PanelKind kind() const override { return PanelKind::Tab; }
    bool available(const EditorContext& ctx) const override;
    // Keeps the editor state and the output manager.
    void onAttach(EditorContext& ctx) override;
    // Selecting an output shows it in the Properties window.
    void drawContent(EditorContext& ctx) override;

private:
    void renderContent();

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

    EditorState* es_ = nullptr;
    OutputManager* outputManager_ = nullptr;

    // State for renaming
    bool isRenaming_ = false;
    int renamingIndex_ = -1;
    char renameBuffer_[128] = "";
};

} // namespace quadmapper
