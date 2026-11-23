#include "ExportVideoWindow.hpp"

#include "core/FileSystem.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorServices.hpp"
#include "editor/SourceAudio.hpp"
#include "editor/VideoExportPlan.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "project/Project.hpp"
#include "source/SourceManager.hpp"
#include "source/TextureSource.hpp"

#include <imgui.h>

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace quadmapper {

namespace {

const ImVec4 ERROR_COLOUR(1.0f, 0.45f, 0.4f, 1.0f);  // as the Missing Media window
constexpr int FPS_CHOICES[] = {25, 30, 50, 60};
constexpr const char* FPS_LABELS[] = {"25", "30", "50", "60"};

VideoExportStatus statusOf(const EditorContext& ctx) {
    return ctx.services ? ctx.services->videoExportStatus() : VideoExportStatus{};
}

const Scene* sceneById(const EditorState& es, const UUID& id) {
    for (const auto& scene : es.scenes) {
        if (scene && scene->getId() == id) return scene.get();
    }
    return nullptr;
}

std::string clockText(double seconds) {
    const int total = static_cast<int>(seconds + 0.5);
    char text[32];
    std::snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}

}  // namespace

int ExportVideoWindow::windowFlags() const {
    return ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
}

void ExportVideoWindow::tick(EditorContext& ctx) {
    std::vector<std::string> picked;
    if (folderDialog_.takeResult(picked) && !picked.empty()) folder_ = picked.front();
    // An export keeps the window open: Cancel lives here.
    if (!visible && statusOf(ctx).active) visible = true;
}

void ExportVideoWindow::beforeBegin(EditorContext& ctx) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 0.0f), ImVec2(900.0f, FLT_MAX));
    // Defaults from the project, once: its folder and name.
    if (!baseNameSet_) {
        baseNameSet_ = true;
        std::string name = ctx.project ? ctx.project->getName() : std::string();
        if (ctx.project && !ctx.project->getFilePath().empty()) {
            const std::filesystem::path file = pathFromUtf8(ctx.project->getFilePath());
            if (folder_.empty()) folder_ = utf8FromPath(file.parent_path());
            if (name.empty()) name = utf8FromPath(file.stem());
        }
        if (name.empty()) name = "QUADmapper";
        std::snprintf(baseName_, sizeof(baseName_), "%s", name.c_str());
    }
}

void ExportVideoWindow::drawContent(EditorContext& ctx) {
    if (statusOf(ctx).active) drawProgress(ctx);
    else drawSettings(ctx);
}

void ExportVideoWindow::drawProgress(EditorContext& ctx) {
    const VideoExportStatus status = statusOf(ctx);
    ImGui::TextUnformatted("Exporting. The projectors show the frames being rendered.");
    char label[32];
    std::snprintf(label, sizeof(label), "%.0f%%", status.progress * 100.0f);
    ImGui::ProgressBar(status.progress, ImVec2(-1.0f, 0.0f), label);
    ImGui::TextDisabled("%s", status.message.c_str());
    if (ImGui::Button("Cancel") && ctx.services) ctx.services->cancelVideoExport();
}

void ExportVideoWindow::drawSettings(EditorContext& ctx) {
    EditorState& es = ctx.es;
    OutputManager* om = ctx.outputManager;
    SourceManager* sm = ctx.sourceManager;
    const VideoExportStatus last = statusOf(ctx);

    // The result of the last export, until the next one starts.
    if (last.finished) {
        if (last.failed) {
            ImGui::PushStyleColor(ImGuiCol_Text, ERROR_COLOUR);
            ImGui::TextWrapped("Export failed: %s", last.message.c_str());
            ImGui::PopStyleColor();
        } else if (last.cancelled) {
            ImGui::TextWrapped("%s", last.message.c_str());
        } else {
            ImGui::TextUnformatted("Written:");
            for (const auto& path : last.written) ImGui::BulletText("%s", utf8FromPath(path).c_str());
        }
        ImGui::Separator();
    }

    if (!om || om->getOutputs().empty()) {
        ImGui::TextWrapped("There are no outputs to export. Add one in the Output panel.");
        return;
    }

    // ---- Outputs ----
    ImGui::TextUnformatted("Outputs (one file each, at the output's resolution and rotation)");
    std::unordered_set<UUID> sceneIds;
    std::vector<UUID> outputIds;
    for (const auto& output : om->getOutputs()) {
        if (!output) continue;
        auto it = chosen_.find(output->getId());
        if (it == chosen_.end()) it = chosen_.emplace(output->getId(), output->isEnabled()).first;
        const Scene* scene = sceneById(es, output->getSceneId());
        const IVec2 size = OutputManager::exportFrameSize(*output);
        char label[256];
        std::snprintf(label, sizeof(label), "%s  (%dx%d, %s)##out%s", output->getName().c_str(), size.x, size.y,
                      scene ? scene->getName().c_str() : "no scene", output->getId().toString().c_str());
        ImGui::Checkbox(label, &it->second);
        if (it->second) {
            outputIds.push_back(output->getId());
            if (scene) sceneIds.insert(scene->getId());
        }
    }

    // ---- Length ----
    const std::unordered_set<UUID> media = mediaUsedByScenes(es.scenes, sceneIds);
    const std::optional<double> videoSeconds = singleVideoSeconds(media, [&](const UUID& id) -> std::optional<double> {
        const auto* playable = sm ? sm->getSourceAs<PlayableSource>(id) : nullptr;
        if (!playable || !playable->isReady()) return std::nullopt;
        return playingSeconds(playable->getDuration(), playable->getSpeed());
    });
    ImGui::Spacing();
    ImGui::TextUnformatted("Length");
    if (videoSeconds) {
        char label[96];
        std::snprintf(label, sizeof(label), "The video's length (%s)", clockText(*videoSeconds).c_str());
        if (ImGui::RadioButton(label, useVideoLength_)) useVideoLength_ = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("Set length", !useVideoLength_)) useVideoLength_ = false;
    }
    const bool customLength = !videoSeconds || !useVideoLength_;
    if (customLength) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::DragFloat("seconds##length", &seconds_, 0.5f, 0.1f, 36000.0f, "%.1f s", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", clockText(seconds_).c_str());
        if (!videoSeconds && !media.empty()) {
            ImGui::TextDisabled("Several videos (or none) are mapped, so set the length.");
        }
    }
    const double seconds = customLength ? static_cast<double>(seconds_) : *videoSeconds;

    // ---- Format ----
    ImGui::Spacing();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::Combo("Frames per second", &fpsIndex_, FPS_LABELS, IM_ARRAYSIZE(FPS_LABELS));
    ImGui::Checkbox("Sound (the mapped videos' audio)", &audio_);
    ImGui::TextDisabled("H.264 MP4: plays on most media players. Only sound at 1x speed forward is heard.");

    // ---- Where ----
    ImGui::Spacing();
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputText("File name", baseName_, sizeof(baseName_));
    ImGui::TextWrapped("Folder: %s", folder_.empty() ? "(choose one)" : folder_.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(folderDialog_.isBusy());
    if (ImGui::Button("Choose...")) {
        AsyncFileDialog::Request request;
        request.kind = AsyncFileDialog::Kind::PickFolder;
        request.title = "Export the video to";
        request.initialPath = folder_;
        folderDialog_.open(request);
    }
    ImGui::EndDisabled();
    {
        std::vector<std::string> outputNames;
        for (const UUID& id : outputIds) {
            const OutputWindow* output = om->getOutput(id);
            outputNames.push_back(output ? output->getName() : std::string());
        }
        for (const std::string& name : exportFileNames(baseName_, outputNames)) {
            ImGui::BulletText("%s", name.c_str());
        }
    }

    // ---- Export ----
    ImGui::Separator();
    const bool locked = es.performLock.isOn();
    const bool ready = !outputIds.empty() && !folder_.empty() && seconds > 0.0 && ctx.services;
    ImGui::BeginDisabled(!ready || locked);
    if (ImGui::Button("Export")) {
        VideoExportRequest request;
        request.folder = pathFromUtf8(folder_);
        request.baseName = baseName_;
        request.outputIds = outputIds;
        request.fps = FPS_CHOICES[fpsIndex_];
        request.seconds = seconds;
        request.audio = audio_;
        error_.clear();
        if (!ctx.services->startVideoExport(request, error_) && error_.empty()) error_ = "The export did not start.";
    }
    ImGui::EndDisabled();
    if (locked) {
        ImGui::SameLine();
        ImGui::TextDisabled("Turn the perform lock off to export: the projectors show the export while it runs.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) visible = false;
    if (!error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ERROR_COLOUR);
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::PopStyleColor();
    }
}

}  // namespace quadmapper
