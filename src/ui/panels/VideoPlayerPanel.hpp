#pragma once

namespace quadmapper {

struct EditorContext;

// Renders the "Video Player" dockable window (load/unload, play/pause/stop,
// skip ±5s, progress seek, time/frame display, speed + 1x, loop-mode combo,
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
};

}  // namespace quadmapper
