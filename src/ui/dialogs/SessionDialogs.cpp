#include "SessionDialogs.hpp"

#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"

#include <imgui.h>

namespace quadmapper {

void SessionDialogs::drawContent(EditorContext& ctx) {
    using FileAction = EditorState::FileAction;
    EditorState& es = ctx.es;

    constexpr const char* kUnsavedPopup = "Unsaved Changes##session";
    if (es.unsavedPromptFor != FileAction::None && !ImGui::IsPopupOpen(kUnsavedPopup)) {
        ImGui::OpenPopup(kUnsavedPopup);
    }
    if (ImGui::BeginPopupModal(kUnsavedPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This project has unsaved changes.");
        ImGui::TextUnformatted("Save them before continuing?");
        ImGui::Spacing();
        const FileAction deferred = es.unsavedPromptFor;
        if (ImGui::Button("Save")) {
            es.saveBeforeAction = true;
            es.pendingFileAction = deferred;
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save")) {
            es.discardConfirmed = true;
            es.pendingFileAction = deferred;
        }
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (es.pendingFileAction != FileAction::None || cancel) {
            es.unsavedPromptFor = FileAction::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    constexpr const char* kRecoveryPopup = "Recover Unsaved Work##session";
    if (!es.recoveryFilePath.empty() && !ImGui::IsPopupOpen(kRecoveryPopup)) {
        ImGui::OpenPopup(kRecoveryPopup);
    }
    if (ImGui::BeginPopupModal(kRecoveryPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(es.recoveryProjectPath.empty()
            ? "QUADmapper found unsaved work from an untitled project."
            : "QUADmapper found unsaved work newer than this project file.");
        ImGui::TextDisabled("%s", es.recoveryFilePath.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Restore")) {
            es.pendingFileAction = FileAction::RestoreRecovery;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            es.pendingFileAction = FileAction::DiscardRecovery;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace quadmapper
