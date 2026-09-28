#pragma once

// The editor's panels, owned by Application (ADR-051 §1). They hold only view state
// (filters, rename buffers, thumbnails, dialogs); the model state they show and edit,
// selections included, is EditorState's, so editor code never needs a panel.

#include "ui/panels/CalibrationPanel.hpp"
#include "ui/panels/ControlPanel.hpp"
#include "ui/panels/InspectorPanel.hpp"
#include "ui/panels/MainMenuBar.hpp"
#include "ui/panels/MediaBinPanel.hpp"
#include "ui/panels/OutputPanel.hpp"
#include "ui/panels/OutputPreviewPanel.hpp"
#include "ui/panels/QuadListPanel.hpp"
#include "ui/panels/ScenePanel.hpp"
#include "ui/panels/VideoPlayerPanel.hpp"
#include "ui/panels/ViewportEditor.hpp"

namespace quadmapper {

struct EditorState;

struct EditorPanels {
    // `es` must outlive the panels: the media bin and the Outputs tab keep a reference.
    explicit EditorPanels(EditorState& es) : mediaBinPanel(es), outputPanel(es) {}
    EditorPanels(const EditorPanels&) = delete;
    EditorPanels& operator=(const EditorPanels&) = delete;

    InspectorPanel inspectorPanel;
    MainMenuBar mainMenuBar;
    bool mainMenuBarInitialized = false;
    MediaBinPanel mediaBinPanel;
    bool mediaBinInitialized = false;
    OutputPanel outputPanel;
    bool outputPanelInitialized = false;
    QuadListPanel quadListPanel;
    ScenePanel scenePanel;
    OutputPreviewPanel outputPreviewPanel;
    VideoPlayerPanel videoPlayerPanel;
    ViewportEditor viewportEditor;  // Non-Panel: pre-NewFrame on-canvas warp surface (ADR-0034)
    CalibrationPanel calibrationPanel;  // shown while calibrating a projector (ADR-052)
    ControlPanel controlPanel;          // OSC / MIDI external control (ADR-048)
};

}  // namespace quadmapper
