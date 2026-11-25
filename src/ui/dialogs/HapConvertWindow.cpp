#include "HapConvertWindow.hpp"
#include "editor/KeyLabels.hpp"

#include "core/FileSystem.hpp"
#include "video/HapExport.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace quadmapper {

namespace {

const ImVec4 ERROR_COLOUR(1.0f, 0.45f, 0.4f, 1.0f);  // as the Missing Media window

const char* variantName(hap::HapVariant variant) {
    return variant == hap::HapVariant::Hap ? "HAP" : "HAP Q";
}

}  // namespace

HapConvertWindow::HapConvertWindow() = default;
HapConvertWindow::~HapConvertWindow() = default;

bool HapConvertWindow::isConverting(const std::filesystem::path& source, hap::HapVariant variant) const {
    return std::any_of(jobs_.begin(), jobs_.end(), [&](const Job& job) {
        return !job.conversion->finished() && job.conversion->source() == source &&
               job.conversion->variant() == variant;
    });
}

void HapConvertWindow::start(const std::filesystem::path& source, hap::HapVariant variant) {
    if (isConverting(source, variant)) return;  // it would pick the same output name
    Job job;
    job.conversion = std::make_unique<HapConversion>(source, variant);
    jobs_.push_back(std::move(job));
}

void HapConvertWindow::render(const AddToBin& addToBin, const ReplaceInShow& replaceInShow,
                              bool showLocked) {
    // A finished file joins the media bin whether or not the window is looked at.
    for (Job& job : jobs_) {
        const HapConversion& c = *job.conversion;
        if (!job.addedToBin && c.finished() && c.result().status == HapExportResult::Status::Done) {
            job.addedToBin = true;
            if (addToBin) addToBin(c.result().output);
        }
    }
    if (jobs_.empty()) return;

    const bool running = std::any_of(jobs_.begin(), jobs_.end(),
                                     [](const Job& job) { return !job.conversion->finished(); });
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    bool open = true;
    // No close button while converting: the rows are the only way to cancel. It does
    // not take focus when it appears, so the canvas keeps the vertex keys.
    if (ImGui::Begin(WINDOW_ID, running ? nullptr : &open,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        size_t dismiss = jobs_.size();
        for (size_t i = 0; i < jobs_.size(); ++i) {
            Job& job = jobs_[i];
            const HapConversion& c = *job.conversion;
            ImGui::PushID(static_cast<int>(i));
            if (i > 0) ImGui::Separator();
            ImGui::Text("%s  ->  %s (%s)", utf8FromPath(c.source().filename()).c_str(),
                        utf8FromPath(c.plannedOutput().filename()).c_str(), variantName(c.variant()));

            if (!c.finished()) {
                const float fraction = static_cast<float>(c.progress());
                char label[32];
                std::snprintf(label, sizeof(label), "%.0f%%", fraction * 100.0f);
                ImGui::ProgressBar(fraction, ImVec2(440.0f, 0.0f), label);
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) job.conversion->cancel();
            } else {
                const HapExportResult& r = c.result();
                ImGui::PushTextWrapPos(540.0f);
                if (r.status == HapExportResult::Status::Done) {
                    ImGui::Text("Done: %s, %lld frames. Added to the media bin.",
                                utf8FromPath(r.output.filename()).c_str(), static_cast<long long>(r.frames));
                    if (!r.audioNote.empty()) ImGui::TextDisabled("%s", r.audioNote.c_str());
                    if (!job.replaceError.empty()) ImGui::TextColored(ERROR_COLOUR, "%s", job.replaceError.c_str());
                    ImGui::BeginDisabled(job.replaced || showLocked);
                    if (ImGui::Button(job.replaced ? "Replaced" : "Replace in show") && replaceInShow) {
                        job.replaceError = replaceInShow(c.source(), r.output);
                        job.replaced = job.replaceError.empty();
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        if (showLocked && !job.replaced) {
                            ImGui::SetTooltip("The show is locked (Output > Perform Lock): unlock it to "
                                              "replace %s.", utf8FromPath(c.source().filename()).c_str());
                        } else {
                            ImGui::SetTooltip("Every quad and 3D object showing %s shows the HAP file instead,\n"
                                              "playing on from the same place. Undo (" QM_KEY_CTRL "+Z) reverts it.",
                                              utf8FromPath(c.source().filename()).c_str());
                        }
                    }
                    ImGui::SameLine();
                } else if (r.status == HapExportResult::Status::Cancelled) {
                    ImGui::TextDisabled("Cancelled. Nothing was written.");
                } else {
                    ImGui::TextColored(ERROR_COLOUR, "Failed: %s", r.error.c_str());
                }
                ImGui::PopTextWrapPos();
                if (ImGui::Button("Dismiss")) dismiss = i;
            }
            ImGui::PopID();
        }
        if (dismiss < jobs_.size()) jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(dismiss));
    }
    ImGui::End();
    if (!open) jobs_.clear();  // only offered once every conversion has finished
}

} // namespace quadmapper
