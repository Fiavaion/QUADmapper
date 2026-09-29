#pragma once

// The editor's user interface (ADR-051 §1–2): the canvas (ViewportEditor), the dock
// layout, the main menu bar, the canvas overlay (zoom, pan, media drop), and every
// panel, drawn through one PanelHost. Application owns it and calls, once a frame:
//
//   NewFrame -> ui.beginFrame(ctx)  (canvas keys, canvas, dockspace, menu, overlay)
//            -> [app: shortcuts, then the queued scene takes]
//            -> ui.drawPanels(ctx) -> ui.drawOverlays(ctx)
//            -> [app: bench hook, projector frames, ImGui::Render]
//
// The canvas runs straight after NewFrame, ahead of every panel, so a vertex nudge acts
// on this frame's input and shows in the next presented frame (TD-21).

#include "ui/PanelHost.hpp"
#include "ui/dialogs/MissingMediaWindow.hpp"
#include "ui/dialogs/SessionDialogs.hpp"
#include "ui/panels/Banners.hpp"
#include "ui/panels/CalibrationPanel.hpp"
#include "ui/panels/ControlPanel.hpp"
#include "ui/panels/InspectorPanel.hpp"
#include "ui/panels/MainMenuBar.hpp"
#include "ui/panels/MediaBinPanel.hpp"
#include "ui/panels/OutputPanel.hpp"
#include "ui/panels/OutputPreviewPanel.hpp"
#include "ui/panels/QuadListPanel.hpp"
#include "ui/panels/ResourcesPanel.hpp"
#include "ui/panels/ScenePanel.hpp"
#include "ui/panels/VideoPlayerPanel.hpp"
#include "ui/panels/ViewportEditor.hpp"

namespace quadmapper {

struct EditorContext;
class PreferencesDialog;

class EditorUi {
public:
    // `preferences` holds the machine preferences, which the app loads before the UI
    // exists; EditorUi draws it as its Preferences window. It must outlive EditorUi.
    explicit EditorUi(PreferencesDialog& preferences);
    ~EditorUi();
    EditorUi(const EditorUi&) = delete;
    EditorUi& operator=(const EditorUi&) = delete;

    // Once, when the components in `ctx` exist.
    void attach(EditorContext& ctx);
    // After New, Open or Restore replaced the show.
    void projectReplaced(EditorContext& ctx);

    // Right after ImGui::NewFrame(): claims the canvas keys, runs the canvas, then the
    // dockspace (with the first-run layout), the menu bar and the canvas overlay.
    void beginFrame(EditorContext& ctx);
    // The windows (Resources with its tabs), after the shortcuts and scene takes.
    void drawPanels(EditorContext& ctx);
    // Calibration, the banners, Convert to HAP, then the session modals, on top.
    void drawOverlays(EditorContext& ctx);

    PanelHost& panels() { return host_; }

private:
    void drawDockSpace(EditorContext& ctx);
    void drawCanvasOverlay(EditorContext& ctx);
    // The UI mode and the audio device are machine preferences (ADR-042, ADR-050 §5),
    // changed from the menu bar and the Preferences window: save a change.
    void savePreferenceChanges(EditorContext& ctx);

    // Convert to HAP progress (QM-082): the media bin's conversions, drawn every frame
    // whichever tab is showing.
    class Conversions : public Panel {
    public:
        explicit Conversions(MediaBinPanel& bin) : bin_(bin) {}
        const char* id() const override { return "Convert to HAP##hapconvert"; }
        const char* menuLabel() const override { return nullptr; }
        PanelKind kind() const override { return PanelKind::Overlay; }
        void drawContent(EditorContext&) override { bin_.renderConversions(); }

    private:
        MediaBinPanel& bin_;
    };

    PanelHost host_;
    ViewportEditor viewport_;  // not a Panel: runs before the dockspace (TD-21)
    MainMenuBar menuBar_;      // not a Panel: the menu bar is not a window
    bool dockLayoutBuilt_ = false;

    // In draw order (PanelHost keeps it).
    QuadListPanel quadList_;
    InspectorPanel inspector_;
    ResourcesPanel resources_{host_};
    ProjectTab projectTab_;
    MediaBinPanel mediaBin_;
    ScenePanel scenes_;
    OutputPreviewPanel preview_;
    OutputPanel outputs_;
    VideoPlayerPanel videoPlayer_;
    ControlPanel control_;
    MissingMediaWindow missingMedia_;
    CalibrationPanel calibration_;
    Banners banners_;
    Conversions conversions_{mediaBin_};
    SessionDialogs sessionDialogs_;
};

}  // namespace quadmapper
