#include "ActionRegistry.hpp"

#include "input/VertexKeyboard.hpp"  // GRID_HANDLE_KEY; ImGuiKey values only, no ImGui calls

namespace quadmapper {

bool isReservedCanvasKey(int key) {
    switch (key) {
        case ImGuiKey_Tab:
        case ImGuiKey_UpArrow:
        case ImGuiKey_DownArrow:
        case ImGuiKey_LeftArrow:
        case ImGuiKey_RightArrow:
        case GRID_HANDLE_KEY:
            return true;
        default:
            return false;
    }
}

AddResult ActionRegistry::add(Action action) {
    if (action.id.empty() || !action.run) return AddResult::Invalid;
    if (action.chord && action.chord->key == ImGuiKey_None) return AddResult::Invalid;
    if (find(action.id)) return AddResult::DuplicateId;
    if (action.chord) {
        if (isReservedCanvasKey(action.chord->key)) return AddResult::ReservedKey;
        if (findByChord(*action.chord)) return AddResult::ChordClash;
    }
    m_actions.push_back(std::move(action));
    return AddResult::Added;
}

Outcome ActionRegistry::invoke(const std::string& id, EditorContext& ctx, Source source) {
    const Action* action = find(id);
    if (!action) return Outcome::Unknown;
    if (m_lockPolicy && m_lockPolicy(*action, source)) return Outcome::Locked;
    // Only Show-class actions may be driven by OSC/MIDI (ADR-051 §3).
    if (source == Source::Control && action->cls != ActionClass::Show) return Outcome::Disabled;
    if (action->enabled && !action->enabled(ctx)) return Outcome::Disabled;
    action->run(ctx);
    return Outcome::Ran;
}

const Action* ActionRegistry::find(const std::string& id) const {
    for (const Action& action : m_actions) {
        if (action.id == id) return &action;
    }
    return nullptr;
}

const Action* ActionRegistry::findByChord(const Chord& chord) const {
    for (const Action& action : m_actions) {
        if (action.chord && *action.chord == chord) return &action;
    }
    return nullptr;
}

}  // namespace quadmapper
