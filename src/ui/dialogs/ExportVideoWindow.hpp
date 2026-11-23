#pragma once

#include "editor/EditorState.hpp"
#include "ui/AsyncFileDialog.hpp"
#include "ui/panels/Panel.hpp"

#include <string>
#include <unordered_map>

namespace quadmapper {

// File > Export Video... (ADR-060): renders the show to one H.264 MP4 per chosen
// output, for a standalone media player. The window picks the outputs, the length
// (the video's own when only one video is mapped), the frame rate, the sound and the
// folder, then shows the export's progress with Cancel. A plain window: the export
// works on a snapshot, so the show stays editable meanwhile, and the projectors show
// the frames being exported.
class ExportVideoWindow : public Panel {
public:
    static constexpr const char* ID = EditorState::EXPORT_VIDEO_PANEL_ID;

    ExportVideoWindow() { visible = false; }

    const char* id() const override { return ID; }
    const char* menuLabel() const override { return nullptr; }  // File > Export Video...
    bool editsShow() const override { return false; }  // it checks the perform lock itself
    int windowFlags() const override;
    void tick(EditorContext& ctx) override;
    void beforeBegin(EditorContext& ctx) override;
    void drawContent(EditorContext& ctx) override;

private:
    void drawSettings(EditorContext& ctx);
    void drawProgress(EditorContext& ctx);

    std::unordered_map<UUID, bool> chosen_;  // output id -> exported (default: enabled ones)
    bool useVideoLength_ = true;
    float seconds_ = 10.0f;
    int fpsIndex_ = 1;                       // 25, 30, 50, 60
    bool audio_ = true;
    std::string folder_;
    char baseName_[128] = "";
    bool baseNameSet_ = false;
    std::string error_;
    AsyncFileDialog folderDialog_;
};

}  // namespace quadmapper
