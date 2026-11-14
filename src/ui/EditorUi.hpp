#pragma once

// The editor's user interface (ADR-051 §1–2): the canvas (ViewportEditor), the dock
// layout, the main menu bar, the canvas overlay (zoom, pan, media drop), and every
// panel, drawn through one PanelHost. Application owns it and, once a frame, calls
// NewFrame -> ui.frame(ctx, takes) -> [bench hook, projector frames, ImGui::Render].
// frame() runs, in this order (FrameStep):
//
//   [locked: a held drag let go] -> canvas keys claimed -> canvas (ViewportEditor)
//   -> dockspace -> menu bar -> canvas overlay -> shortcuts (ShortcutRouter)
//   -> the app's queued scene takes -> panels -> overlays (calibration, banners,
//   session modals)
//
// The canvas runs straight after NewFrame, ahead of every panel, so a vertex nudge acts
// on this frame's input and shows in the next presented frame (TD-21). The shortcuts
// and takes run before the panels, so the panels show this frame's state.

#include "ui/PanelHost.hpp"
#include "ui/dialogs/MissingMediaWindow.hpp"
#include "ui/dialogs/SessionDialogs.hpp"
#include "ui/panels/AiPhotoPanel.hpp"
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

#include <functional>

namespace quadmapper {

struct EditorContext;
class PreferencesDialog;
enum class CanvasKeyOwner;

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

    // The steps of frame(), in order.
    enum class FrameStep {
        CanvasKeys,     // the canvas claims Tab / the arrows / H when it edits
        Canvas,         // ViewportEditor
        DockSpace,      // with the default layout when none is saved
        MenuBar,        // and the status bar
        CanvasOverlay,  // zoom, pan and media drop
        Shortcuts,      // ctx.actions through the ShortcutRouter
        SceneTakes,     // `sceneTakes`
        Panels,         // the windows (Resources with its tabs)
        Overlays,       // calibration, the banners, Convert to HAP, the session modals
    };

    // One editor frame, right after ImGui::NewFrame(). `sceneTakes` runs the scene
    // takes queued since the last frame.
    void frame(EditorContext& ctx, const std::function<void(EditorContext&)>& sceneTakes);

    // Tests only: called as each step of frame() finishes.
    std::function<void(FrameStep)> onFrameStep;

    PanelHost& panels() { return host_; }

private:
    void stepDone(FrameStep step) {
        if (onFrameStep) onFrameStep(step);
    }
    void drawDockSpace(EditorContext& ctx);
    // The strip along the bottom of the editor: the latest status message while it is
    // fresh, otherwise the keys that act on the canvas now. Shown in both UI modes.
    void drawStatusBar(EditorContext& ctx, CanvasKeyOwner keys);
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
        // Cancel stays usable under the perform lock; the window disables "Replace in
        // show" itself (HapConvertWindow::render).
        bool editsShow() const override { return false; }
        void drawContent(EditorContext&) override { bin_.renderConversions(); }

    private:
        MediaBinPanel& bin_;
    };

    PanelHost host_;
    ViewportEditor viewport_;  // not a Panel: runs before the dockspace (TD-21)
    MainMenuBar menuBar_;      // not a Panel: the menu bar is not a window
    // The first frame looked for a saved dock layout (and built the default without one).
    bool dockLayoutChecked_ = false;
    // Frames until the default layout's front tabs are chosen; the tabs exist one frame
    // after the layout is built.
    int frontTabsIn_ = 0;
    bool canvasPanning_ = false;  // a middle-button drag that began on the canvas

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
    AiPhotoPanel aiPhoto_;
    MissingMediaWindow missingMedia_;
    CalibrationPanel calibration_;
    Banners banners_;
    Conversions conversions_{mediaBin_};
    SessionDialogs sessionDialogs_;
};

}  // namespace quadmapper
