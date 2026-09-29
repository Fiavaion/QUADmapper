#pragma once

#include <string>
#include <vector>

namespace quadmapper {

struct EditorContext;
class AudioEngine;
class PanelHost;

// Renders the application main menu bar (File / Edit / Window / Output / AI).
// Not a Panel: it is always shown and manages its own ImGui::BeginMainMenuBar /
// EndMainMenuBar lifetime (ADR-051 §2). EditorUi owns it.
// File items (New / Open / Save / Save As / Exit) set ctx.es.pendingFileAction,
// which Application runs at the next frame boundary (ADR-046). The Window menu is
// generated from `panels`; windows are opened through EditorState::requestPanelOpen.
// The AI menu (ADR-035) goes through ctx.services and degrades cleanly without it.
class MainMenuBar {
public:
    // Call once per frame inside the ImGui frame, after the dockspace.
    void render(EditorContext& ctx, PanelHost& panels);

private:
    // Master sound (ADR-050 §2): mute, volume and the output device, beside the master
    // timeline so the show's transport and sound live in one strip.
    void renderAudioStrip(AudioEngine& audio);
    // Output-device choices; `menu` renders menu items, otherwise combo entries.
    // `listOpen` is that list's was-open-last-frame flag: the devices are enumerated
    // once when it opens, not every frame.
    void renderAudioDevices(AudioEngine& audio, bool menu, bool& listOpen);

    std::vector<std::string> audioDevices_;
    bool audioComboOpen_ = false;  // the strip's device combo
    bool audioMenuOpen_ = false;   // Output > Audio Output
};

}  // namespace quadmapper
