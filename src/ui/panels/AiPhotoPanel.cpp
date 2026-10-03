#include "AiPhotoPanel.hpp"

#include "core/FileSystem.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorServices.hpp"
#include "renderer/Texture.hpp"

#include <imgui.h>

#include <cstdint>
#include <vector>

namespace quadmapper {

namespace {

constexpr ImU32 OUTLINE_COLOR = IM_COL32(0, 220, 255, 255);
constexpr ImU32 CORNER_COLOR = IM_COL32(255, 210, 0, 255);
constexpr ImU32 STUB_COLOR = IM_COL32(160, 160, 160, 255);
constexpr ImVec4 WARNING_COLOR(1.0f, 0.7f, 0.3f, 1.0f);
constexpr const char* CORNER_LABELS[4] = {"TL", "TR", "BR", "BL"};

}  // namespace

AiPhotoPanel::AiPhotoPanel() { visible = false; }  // AI > Pick Surface... opens it

AiPhotoPanel::~AiPhotoPanel() = default;

void AiPhotoPanel::drawContent(EditorContext& ctx) {
    using AiAction = EditorServices::AiAction;
    EditorServices* services = ctx.services;
    if (!services) {
        ImGui::TextDisabled("AI assist is not available.");
        return;
    }
    const AiPhoto& photo = services->aiPhoto();
    const bool busy = services->aiBusy();
    const bool canSegment = services->aiActive() && services->aiCanSegment();
    const std::optional<AiSurfaceProposal>& proposal = photo.proposal;

    if (photo.serial != loadedSerial_) {
        loadedSerial_ = photo.serial;
        photo_.reset();
        if (!photo.path.empty()) {
            auto texture = std::make_unique<Texture>();
            if (texture->loadFromFile(pathFromUtf8(photo.path))) photo_ = std::move(texture);
        }
    }

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Take Photo")) services->runAi(AiAction::TakePhoto);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Photograph the stage with the first camera");
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !proposal || !proposal->canApply());
    if (ImGui::Button("Apply")) services->runAi(AiAction::ApplyProposal);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Warp the selected quad onto the proposed surface");
    ImGui::SameLine();
    ImGui::BeginDisabled(!proposal);
    if (ImGui::Button("Clear")) services->runAi(AiAction::ClearProposal);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Discard the proposal");

    if (!services->aiActive()) {
        ImGui::TextDisabled("Enable the sidecar (AI > Enable AI Assist) to pick a surface.");
    } else if (!canSegment) {
        ImGui::TextDisabled("This AI sidecar cannot segment: update it.");
    } else if (busy) {
        ImGui::TextDisabled("Working...");
    } else {
        ImGui::TextDisabled("Click the surface you want in the photo.");
    }

    if (proposal) {
        ImGui::Text("Confidence %.2f", proposal->confidence);
        ImGui::SameLine();
        if (proposal->planarity) {
            ImGui::Text("Planarity %.2f", *proposal->planarity);
        } else {
            ImGui::TextDisabled("Planarity n/a");
        }
        if (proposal->stub) {
            ImGui::TextColored(WARNING_COLOR, "Placeholder only: the sidecar has no SAM 2 model.");
        } else if (!proposal->canApply()) {
            ImGui::TextColored(WARNING_COLOR, "Too uncertain to apply (below %.2f).",
                               AI_MIN_CONFIDENCE);
        }
    }

    if (!photo_) {
        ImGui::TextDisabled(photo.path.empty() ? "No photo yet: press Take Photo."
                                               : "The photo could not be loaded.");
        return;
    }

    // The photo fits the panel, keeping its aspect.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float aspect = static_cast<float>(photo_->getHeight()) /
                         static_cast<float>(photo_->getWidth());
    ImVec2 size(avail.x, avail.x * aspect);
    if (size.y > avail.y && avail.y > 64.0f) size = ImVec2(avail.y / aspect, avail.y);
    if (size.x < 1.0f || size.y < 1.0f) return;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##photo", size);
    if (canSegment && !busy) ImGui::SetItemTooltip("Click the surface to map");
    if (clicked && canSegment && !busy) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        services->aiSegmentAt((mouse.x - origin.x) / size.x, (mouse.y - origin.y) / size.y);
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    // Texture rows are top-down (no flip on load), as ImGui samples them.
    draw->AddImage(static_cast<ImTextureID>(static_cast<uintptr_t>(photo_->getId())), origin, end);
    if (!proposal) return;

    const auto toScreen = [&](const std::array<float, 2>& p) {
        return ImVec2(origin.x + p[0] * size.x, origin.y + p[1] * size.y);
    };
    if (proposal->outline.size() >= 2) {
        std::vector<ImVec2> outline;
        outline.reserve(proposal->outline.size());
        for (const auto& p : proposal->outline) outline.push_back(toScreen(p));
        draw->AddPolyline(outline.data(), static_cast<int>(outline.size()),
                          proposal->stub ? STUB_COLOR : OUTLINE_COLOR, ImDrawFlags_Closed, 2.0f);
    }
    ImVec2 corners[4];
    for (int i = 0; i < 4; ++i) corners[i] = toScreen(proposal->corners[i]);
    const ImU32 cornerColor = proposal->stub ? STUB_COLOR : CORNER_COLOR;
    draw->AddPolyline(corners, 4, cornerColor, ImDrawFlags_Closed, 1.5f);
    for (int i = 0; i < 4; ++i) {
        draw->AddCircleFilled(corners[i], 5.0f, cornerColor);
        draw->AddText(ImVec2(corners[i].x + 7.0f, corners[i].y - 7.0f), cornerColor, CORNER_LABELS[i]);
    }
}

}  // namespace quadmapper
