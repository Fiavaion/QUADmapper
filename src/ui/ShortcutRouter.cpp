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

std::vector<std::string> routeShortcuts(ActionRegistry& registry, EditorContext& ctx) {
    std::vector<std::string> invoked;
    // The Keyboard preferences are capturing a chord: the key is theirs.
    if (ctx.es.capturingChord) return invoked;
    const uint8_t mods = currentChordMods();
    for (const Action& action : registry.actions()) {
        for (const Chord& chord : action.chords) {
            if (chord.mods != mods) continue;
            if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(chord.key), false)) continue;
            if (keyGuardAllows(action.guard)) invoked.push_back(action.id);
            break;
        }
    }
    // Invoke after the scan: an action may add to the registry.
    for (const std::string& id : invoked) registry.invoke(id, ctx, Source::Key);
    return invoked;
}

void runSceneTakeKeys(ActionRegistry& registry, EditorContext& ctx, std::vector<int>& numbers) {
    // A digit typed into a text field, or pressed as a chord being captured, is not a take.
    const bool typing = ImGui::GetIO().WantTextInput || ctx.es.capturingChord;
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
