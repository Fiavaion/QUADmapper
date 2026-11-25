#include "ActionRegistry.hpp"

#include "core/Logger.hpp"
#include "input/VertexKeyboard.hpp"  // GRID_HANDLE_KEY; ImGuiKey values only, no ImGui calls

#include <algorithm>
#include <cctype>

namespace quadmapper {

namespace {

struct KeyName {
    int key;
    const char* name;
};

// The keys a chord can name, with the names the menu shows and the preferences file
// stores. Letters, digits, F1-F24 and the keypad digits are generated (keyName).
constexpr KeyName NAMED_KEYS[] = {
    {ImGuiKey_Tab, "Tab"},
    {ImGuiKey_LeftArrow, "LeftArrow"},
    {ImGuiKey_RightArrow, "RightArrow"},
    {ImGuiKey_UpArrow, "UpArrow"},
    {ImGuiKey_DownArrow, "DownArrow"},
    {ImGuiKey_PageUp, "PageUp"},
    {ImGuiKey_PageDown, "PageDown"},
    {ImGuiKey_Home, "Home"},
    {ImGuiKey_End, "End"},
    {ImGuiKey_Insert, "Insert"},
    {ImGuiKey_Delete, "Delete"},
    {ImGuiKey_Backspace, "Backspace"},
    {ImGuiKey_Space, "Space"},
    {ImGuiKey_Enter, "Enter"},
    {ImGuiKey_Escape, "Escape"},
    {ImGuiKey_Apostrophe, "'"},
    {ImGuiKey_Comma, ","},
    {ImGuiKey_Minus, "-"},
    {ImGuiKey_Period, "."},
    {ImGuiKey_Slash, "/"},
    {ImGuiKey_Semicolon, ";"},
    {ImGuiKey_Equal, "="},
    {ImGuiKey_LeftBracket, "["},
    {ImGuiKey_Backslash, "\\"},
    {ImGuiKey_RightBracket, "]"},
    {ImGuiKey_GraveAccent, "`"},
    {ImGuiKey_KeypadDecimal, "KeypadDecimal"},
    {ImGuiKey_KeypadDivide, "KeypadDivide"},
    {ImGuiKey_KeypadMultiply, "KeypadMultiply"},
    {ImGuiKey_KeypadSubtract, "KeypadSubtract"},
    {ImGuiKey_KeypadAdd, "KeypadAdd"},
    {ImGuiKey_KeypadEnter, "KeypadEnter"},
    {ImGuiKey_KeypadEqual, "KeypadEqual"},
};

// The key's name, or empty for a key a chord cannot use.
std::string keyName(int key) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return std::string(1, char('A' + (key - ImGuiKey_A)));
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return std::string(1, char('0' + (key - ImGuiKey_0)));
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) return "F" + std::to_string(key - ImGuiKey_F1 + 1);
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) {
        return "Keypad" + std::to_string(key - ImGuiKey_Keypad0);
    }
    for (const KeyName& named : NAMED_KEYS) {
        if (named.key == key) return named.name;
    }
    return {};
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The key named `name` (case-insensitive), or ImGuiKey_None.
int keyNamed(const std::string& name) {
    const std::string wanted = lower(name);
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        if (isChordKey(key) && lower(keyName(key)) == wanted) return key;
    }
    return ImGuiKey_None;
}

}  // namespace

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

bool bareKeyAllowed(int key) {
    return (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) || key == ImGuiKey_Delete;
}

bool isChordKey(int key) { return !keyName(key).empty(); }

std::string chordText(const Chord& chord) {
    std::string text;
    if (chord.mods & MOD_CTRL) text += "Ctrl+";
    if (chord.mods & MOD_SHIFT) text += "Shift+";
    if (chord.mods & MOD_ALT) text += "Alt+";
    if (chord.mods & MOD_SUPER) text += "Super+";
    const std::string name = keyName(chord.key);
    return text + (name.empty() ? "Key " + std::to_string(chord.key) : name);
}

std::string chordsText(const std::vector<Chord>& chords) {
    std::string text;
    for (const Chord& chord : chords) {
        if (!text.empty()) text += ", ";
        text += chordText(chord);
    }
    return text;
}

std::optional<Chord> parseChord(const std::string& text) {
    // The key is after the last '+': "Ctrl+=" and "Alt+-" have no '+' key to confuse.
    const size_t plus = text.rfind('+');
    const std::string keyPart = plus == std::string::npos ? text : text.substr(plus + 1);
    Chord chord;
    chord.key = keyNamed(keyPart);
    if (chord.key == ImGuiKey_None) return std::nullopt;
    if (plus == std::string::npos) return chord;
    // Each modifier ends at a '+'; the last one at the '+' before the key.
    for (size_t start = 0; start <= plus;) {
        const size_t end = text.find('+', start);
        const std::string mod = lower(text.substr(start, end - start));
        if (mod == "ctrl") chord.mods |= MOD_CTRL;
        else if (mod == "shift") chord.mods |= MOD_SHIFT;
        else if (mod == "alt") chord.mods |= MOD_ALT;
        else if (mod == "super") chord.mods |= MOD_SUPER;
        else return std::nullopt;
        start = end + 1;
    }
    return chord;
}

std::string rebindRefusal(const RebindCheck& check) {
    const std::string chord = chordText(check.chord);
    switch (check.result) {
        case RebindResult::Changed:       return {};
        case RebindResult::UnknownId:     return "There is no such action.";
        case RebindResult::Fixed:         return "This key is fixed: it cannot be changed.";
        case RebindResult::Invalid:       return "That key cannot be used in a shortcut.";
        case RebindResult::ReservedKey:
            return chord + " is kept for the canvas: Tab, the arrows and H move and pick vertices.";
        case RebindResult::NeedsModifier:
            return chord + " needs Ctrl, Shift, Alt or Super: only F1-F24 and Delete work alone.";
        case RebindResult::ChordClash:
            return check.clashWith.empty() ? chord + " is listed twice."
                                           : chord + " is already used by " + check.clashWith + ".";
    }
    return {};
}

AddResult ActionRegistry::add(Action action) {
    if (action.id.empty() || !action.run) return AddResult::Invalid;
    if (find(action.id)) return AddResult::DuplicateId;
    for (size_t i = 0; i < action.chords.size(); ++i) {
        const Chord& chord = action.chords[i];
        if (chord.key == ImGuiKey_None) return AddResult::Invalid;
        if (isReservedCanvasKey(chord.key)) return AddResult::ReservedKey;
        if (findByChord(chord)) return AddResult::ChordClash;
        for (size_t j = 0; j < i; ++j) {
            if (action.chords[j] == chord) return AddResult::ChordClash;
        }
    }
    action.defaultChords = action.chords;
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

Action* ActionRegistry::findMutable(const std::string& id) {
    return const_cast<Action*>(find(id));
}

const Action* ActionRegistry::findByChord(const Chord& chord) const {
    for (const Action& action : m_actions) {
        if (std::find(action.chords.begin(), action.chords.end(), chord) != action.chords.end()) {
            return &action;
        }
    }
    return nullptr;
}

RebindCheck ActionRegistry::setChords(const std::string& id, std::vector<Chord> chords) {
    RebindCheck check;
    Action* action = findMutable(id);
    if (!action) {
        check.result = RebindResult::UnknownId;
        return check;
    }
    if (action->fixed) {
        check.result = RebindResult::Fixed;
        return check;
    }
    for (size_t i = 0; i < chords.size(); ++i) {
        const Chord& chord = chords[i];
        check.chord = chord;
        if (!isChordKey(chord.key)) {
            check.result = RebindResult::Invalid;
            return check;
        }
        if (isReservedCanvasKey(chord.key)) {
            check.result = RebindResult::ReservedKey;
            return check;
        }
        if (chord.mods == MOD_NONE && !bareKeyAllowed(chord.key)) {
            check.result = RebindResult::NeedsModifier;
            return check;
        }
        const Action* other = findByChord(chord);
        if (other && other != action) {
            check.result = RebindResult::ChordClash;
            check.clashWith = other->label;
            return check;
        }
        for (size_t j = 0; j < i; ++j) {
            if (chords[j] == chord) {
                check.result = RebindResult::ChordClash;
                return check;
            }
        }
    }
    action->chords = std::move(chords);
    check = RebindCheck{};
    return check;
}

bool ActionRegistry::resetChords(const std::string& id) {
    Action* action = findMutable(id);
    if (!action || action->fixed) return false;
    action->chords = action->defaultChords;
    return true;
}

void ActionRegistry::resetAllChords() {
    for (Action& action : m_actions) action.chords = action.defaultChords;
}

KeyBindings keyBindingsOf(const ActionRegistry& registry) {
    KeyBindings bindings;
    for (const Action& action : registry.actions()) {
        if (action.fixed || action.chords == action.defaultChords) continue;
        std::vector<std::string>& texts = bindings[action.id];
        for (const Chord& chord : action.chords) texts.push_back(chordText(chord));
    }
    return bindings;
}

std::vector<std::string> applyKeyBindings(ActionRegistry& registry, const KeyBindings& bindings) {
    std::vector<std::string> problems;
    const auto problem = [&problems](std::string text) {
        Logger::warning("Key bindings: {}", text);
        problems.push_back(std::move(text));
    };

    registry.resetAllChords();
    std::vector<std::pair<std::string, std::vector<Chord>>> wanted;
    for (const auto& [id, texts] : bindings) {
        const Action* action = registry.find(id);
        if (!action) {
            problem("ignored \"" + id + "\": no such action");
            continue;
        }
        if (action->fixed) {
            problem("ignored \"" + id + "\": its key is fixed");
            continue;
        }
        std::vector<Chord> chords;
        bool parsed = true;
        for (const std::string& text : texts) {
            const std::optional<Chord> chord = parseChord(text);
            if (!chord) {
                problem("ignored \"" + id + "\": \"" + text + "\" is not a key chord");
                parsed = false;
                break;
            }
            chords.push_back(*chord);
        }
        if (parsed) wanted.emplace_back(id, std::move(chords));
    }

    // Let go first, so an override may take a chord another override gives up.
    for (const auto& entry : wanted) registry.setChords(entry.first, {});
    for (const auto& [id, chords] : wanted) {
        const RebindCheck check = registry.setChords(id, chords);
        if (check.result == RebindResult::Changed) continue;
        problem("ignored \"" + id + "\": " + rebindRefusal(check));
        // Back to its defaults, less any another override now holds.
        std::vector<Chord> defaults = registry.find(id)->defaultChords;
        defaults.erase(std::remove_if(defaults.begin(), defaults.end(),
                                      [&registry](const Chord& c) { return registry.findByChord(c); }),
                       defaults.end());
        if (defaults.size() == registry.find(id)->defaultChords.size()) {
            registry.resetChords(id);
        } else {
            registry.setChords(id, defaults);
        }
    }
    return problems;
}

}  // namespace quadmapper
