#include "MissingMediaWindow.hpp"

#include "core/FileSystem.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorServices.hpp"
#include "editor/MissingMedia.hpp"

#include <imgui.h>

#include <string>
#include <vector>

namespace quadmapper {

namespace {

bool searching(const EditorContext& ctx) {
    return ctx.services && ctx.services->isSearchingMedia();
}

}  // namespace

int MissingMediaWindow::windowFlags() const {
    return ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse;
}

void MissingMediaWindow::tick(EditorContext& ctx) {
    // All found, or the project that had them is gone.
    if (visible && collectMissingMedia(ctx.es).empty() && !searching(ctx)) visible = false;
}

void MissingMediaWindow::beforeBegin(EditorContext& /*ctx*/) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720.0f, 380.0f), ImGuiCond_Appearing);
}

void MissingMediaWindow::drawContent(EditorContext& ctx) {
    EditorState& es = ctx.es;
    EditorServices* services = ctx.services;
    const std::vector<MissingMediaItem> items = collectMissingMedia(es, ctx.sourceManager);
    const bool isSearching = searching(ctx);

    ImGui::TextWrapped("%zu file%s used by this project could not be opened. What uses them "
                       "shows a magenta checker here and black on the projectors until the "
                       "file is found. Saving keeps the original paths, so nothing is lost "
                       "if you skip this.",
                       items.size(), items.size() == 1 ? "" : "s");
    const bool busy = !services || isSearching || services->isFileDialogBusy();
    if (isSearching) ImGui::TextDisabled("Searching %s ...", services->mediaSearchFolder().c_str());
    ImGui::Spacing();

    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##missing", 3, tableFlags, ImVec2(0.0f, -footer))) {
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Used by", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < items.size(); ++i) {
            const MissingMediaItem& item = items[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(utf8FromPath(pathFromUtf8(item.path).filename()).c_str());
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", item.path.c_str());
            auto error = es.missingMediaErrors.find(item.path);
            if (error != es.missingMediaErrors.end()) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error->second.c_str());
            }
            ImGui::PopTextWrapPos();

            ImGui::TableNextColumn();
            std::string usedBy;
            if (item.quadUses > 0) {
                usedBy = std::to_string(item.quadUses) + (item.quadUses == 1 ? " quad" : " quads");
            }
            if (item.objectUses > 0) {
                if (!usedBy.empty()) usedBy += ", ";
                usedBy += std::to_string(item.objectUses) +
                          (item.objectUses == 1 ? " 3D object" : " 3D objects");
            }
            if (item.cueUses > 0) {
                if (!usedBy.empty()) usedBy += ", ";
                usedBy += std::to_string(item.cueUses) + (item.cueUses == 1 ? " cue" : " cues");
            }
            if (item.kind == MissingMediaItem::Kind::Mesh) usedBy += " (mesh)";
            ImGui::TextUnformatted(usedBy.c_str());

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Locate...")) services->requestLocateMissingMedia(item);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Search Folder...")) services->requestMissingMediaSearch();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Pick a folder: every missing file with the same name in it,\n"
                          "or in any folder below it, is relocated.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip")) visible = false;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Keep working without these files. File > Relocate Missing Media...\n"
                          "reopens this window.");
    }
}

}  // namespace quadmapper
