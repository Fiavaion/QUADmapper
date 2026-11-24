#pragma once

#include "Panel.hpp"
#include "core/UUID.hpp"
#include "ui/ConfirmPopup.hpp"
#include <string>

namespace quadmapper {

struct EditorContext;
struct EditorState;
class OutputManager;
class OutputWindow;

/**
 * The Outputs tab of the Resources window, in both UI modes (several projectors are
 * not an Advanced-only need): add/remove outputs, identify them on their projectors,
 * configure resolution/monitor settings, and enter/exit perform mode. The selected
 * output lives in EditorState::selectedOutputId.
 */
class OutputPanel : public Panel {
public:
    static constexpr const char* ID = "Outputs";

    const char* id() const override { return ID; }
    PanelKind kind() const override { return PanelKind::Tab; }
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

    // Ask before removing an output; the removal runs when the modal is confirmed.
    void requestDelete(const UUID& id);
    void renderDeleteConfirm();

    // Render monitor dropdown
    void renderMonitorDropdown(OutputWindow* output);

    // Render resolution controls
    void renderResolutionControls(OutputWindow* output);

    // Render rotation controls
    void renderRotationControls(OutputWindow* output);

    EditorState* es_ = nullptr;
    OutputManager* outputManager_ = nullptr;

    // The custom width/height being typed; applied when the field is left.
    UUID resEditId_;
    bool editingWidth_ = false;
    bool editingHeight_ = false;
    int pendingWidth_ = 0;
    int pendingHeight_ = 0;

    // State for renaming
    bool isRenaming_ = false;
    int renamingIndex_ = -1;
    char renameBuffer_[128] = "";

    ConfirmPopup deleteConfirm_{"outputdelete"};
    UUID pendingDeleteId_;  // the output the open confirmation is about
    UUID menuDeleteId_;     // set by a row's menu, picked up after the list loop
};

} // namespace quadmapper
