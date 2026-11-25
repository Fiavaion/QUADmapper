#pragma once

#include <imgui.h>

#include <cctype>
#include <string>
#include <utility>

namespace quadmapper {

// True when a rename field holds nothing but whitespace: the list panels keep the old
// name in that case.
inline bool isBlankName(const char* name) {
    for (; *name; ++name) {
        if (!std::isspace(static_cast<unsigned char>(*name))) return false;
    }
    return true;
}

// A modal that asks before a destructive action, in the style of "Unlock the Show?":
// the title names the action, the body names the item and the consequence, the
// buttons are the action verb and "Cancel". Keyboard focus starts on Cancel, so a
// stray Enter or Space cancels; Esc cancels too.
//
// The owner keeps what to act on (an id) and calls request() when the user asks,
// then draw() every frame from the same ID stack. draw() returns true on the frame
// the user confirms.
class ConfirmPopup {
public:
    explicit ConfirmPopup(const char* popupId) : popupId_(popupId) {}

    void request(const std::string& title, std::string body, std::string verb) {
        label_ = title + "##" + popupId_;
        body_ = std::move(body);
        verb_ = std::move(verb);
        openRequested_ = true;
    }

    bool draw() {
        if (label_.empty()) return false;
        if (openRequested_) {
            ImGui::OpenPopup(label_.c_str());
            openRequested_ = false;
        }
        bool confirmed = false;
        if (ImGui::BeginPopupModal(label_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(body_.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button(verb_.c_str())) {
                confirmed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            const bool cancel = ImGui::Button("Cancel");
            ImGui::SetItemDefaultFocus();
            if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return confirmed;
    }

private:
    std::string popupId_;
    std::string label_;
    std::string body_;
    std::string verb_;
    bool openRequested_ = false;
};

}  // namespace quadmapper
