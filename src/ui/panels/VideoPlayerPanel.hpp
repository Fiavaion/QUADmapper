#pragma once

#include "ui/AsyncFileDialog.hpp"

namespace quadmapper {

struct EditorContext;

// Renders the "Video Player" dockable window (load/unload, play/pause/stop,
// skip ±5s, progress seek, time/frame display, speed + 1x, loop toggle,
// and assign/remove video to the selected quad). Visibility is controlled by
// ctx.es.showVideoPlayer; when false, render() returns immediately.
//
// Does NOT inherit from Panel — matches the QuadListPanel / MainMenuBar pattern
// where EditorContext is required per frame.
class VideoPlayerPanel {
public:
    VideoPlayerPanel() = default;

    // Call once per frame inside the ImGui frame, after the dockspace.
    void render(EditorContext& ctx);

private:
    // Load es.videoPath as the active video (deduped). In Simple mode it is also
    // bound to the selected quad and started.
    void loadVideo(EditorContext& ctx, bool simpleMode);

    // The "Load Video..." browser runs off the render thread (QM-088).
    AsyncFileDialog loadDialog_;
    bool loadDialogSimpleMode_ = false;  // UI mode the dialog was opened from
};

}  // namespace quadmapper
