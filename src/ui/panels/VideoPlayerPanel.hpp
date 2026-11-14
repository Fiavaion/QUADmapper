#pragma once

#include "Panel.hpp"
#include "core/UUID.hpp"
#include "ui/AsyncFileDialog.hpp"
#include "ui/ConfirmPopup.hpp"

namespace quadmapper {

struct EditorContext;

// The "Video Player" dockable window (load/unload, play/pause/stop, skip ±5s,
// progress seek, time/frame display, speed + 1x, loop toggle, and assign/remove video
// to the selected quad).
class VideoPlayerPanel : public Panel {
public:
    static constexpr const char* ID = "Video Player";

    VideoPlayerPanel() = default;

    const char* id() const override { return ID; }
    // Its transport stays usable under the perform lock; drawContent disables the rest.
    bool editsShow() const override { return false; }
    // Collects a finished "Load Video..." pick and follows the selection, shown or not.
    void tick(EditorContext& ctx) override;
    void drawContent(EditorContext& ctx) override;

    // Load es.videoPath as the active video (deduped). In Simple mode it is also
    // bound to the selected quad and started. The "Load Video..." pick lands here when
    // its dialog closes; under the perform lock it is refused (the button is disabled
    // then, but the dialog is ownerless and may be answered after locking).
    void loadVideo(EditorContext& ctx, bool simpleMode);

private:
    void unloadVideo(EditorContext& ctx, const UUID& unloaded);

    // The "Load Video..." browser runs off the render thread (QM-088).
    AsyncFileDialog loadDialog_;
    bool loadDialogSimpleMode_ = false;  // UI mode the dialog was opened from

    // The selected quad last seen, so the panel follows a selection change only once.
    UUID followedQuadId_;

    // "Unload Video" asks first when more than one quad shows the clip.
    ConfirmPopup unloadConfirm_{"videounload"};
    UUID pendingUnloadId_;
};

}  // namespace quadmapper
