#include "ShortcutRouter.hpp"

#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/StatusLine.hpp"

#include <imgui.h>

namespace quadmapper {

uint8_t currentChordMods() {
    const ImGuiIO& io = ImGui::GetIO();
    uint8_t mods = MOD_NONE;
    if (io.KeyCtrl) mods |= MOD_CTRL;
    if (io.KeyShift) mods |= MOD_SHIFT;
    if (io.KeyAlt) mods |= MOD_ALT;
    if (io.KeySuper) mods |= MOD_SUPER;
    return mods;
}

bool keyGuardAllows(KeyGuard guard) {
    if (ImGui::GetIO().WantTextInput) return false;
    if (guard == KeyGuard::Show) return true;
    return !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

std::string chordText(const Chord& chord) {
    std::string text;
    if (chord.mods & MOD_CTRL) text += "Ctrl+";
    if (chord.mods & MOD_SHIFT) text += "Shift+";
    if (chord.mods & MOD_ALT) text += "Alt+";
    if (chord.mods & MOD_SUPER) text += "Super+";
    switch (chord.key) {
        case ImGuiKey_Comma:        text += ","; break;
        case ImGuiKey_LeftBracket:  text += "["; break;
        case ImGuiKey_RightBracket: text += "]"; break;
        case ImGuiKey_Equal:        text += "="; break;
        case ImGuiKey_Minus:        text += "-"; break;
        default: text += ImGui::GetKeyName(static_cast<ImGuiKey>(chord.key)); break;
    }
    return text;
}

std::vector<std::string> routeShortcuts(ActionRegistry& registry, EditorContext& ctx) {
    std::vector<std::string> invoked;
    const uint8_t mods = currentChordMods();
    for (const Action& action : registry.actions()) {
        if (!action.chord || action.chord->mods != mods) continue;
        if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(action.chord->key), false)) continue;
        if (!keyGuardAllows(action.guard)) continue;
        invoked.push_back(action.id);
    }
    // Invoke after the scan: an action may add to the registry.
    for (const std::string& id : invoked) registry.invoke(id, ctx, Source::Key);
    return invoked;
}

void runSceneTakeKeys(ActionRegistry& registry, EditorContext& ctx, std::vector<int>& numbers) {
    const bool typing = ImGui::GetIO().WantTextInput;
    // The unlock prompt is opened from the top level, which closes every other popup,
    // so while it is asked for it is the only one.
    const bool heldOff = !typing && !ctx.es.performLock.isConfirmingUnlock() &&
                         !keyGuardAllows(KeyGuard::Editing);
    for (int n : numbers) {
        if (typing) continue;
        if (heldOff) {
            postStatus(ctx.es, "Scene " + std::to_string(n) +
                                   " not taken: close the open menu or dialog first");
            continue;
        }
        registry.invoke(sceneTakeId(n), ctx, Source::Key);
    }
    numbers.clear();
}

}  // namespace quadmapper
