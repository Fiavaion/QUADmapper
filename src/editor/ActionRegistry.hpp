#pragma once

// The action registry (ADR-051 §3): every editor-wide command with a key or a menu
// item is one Action, and the keys (ui/ShortcutRouter), the menu and external control
// all invoke it by id, so they cannot disagree. The canvas keys (Tab / Shift+Tab, the
// arrows with Shift 10x and Alt 0.1x, H / Shift+H) stay outside it: they are the
// success metric (input/VertexKeyboard.hpp), and no chord may ever use them.
//
// An action has any number of chords; the first is the one the menu shows. The user
// can rebind every action but the fixed ones (setChords, Edit > Preferences >
// Keyboard); the overrides are saved in the preferences as text ("Ctrl+Shift+L").

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace quadmapper {

struct EditorContext;

// Chord modifiers. Matching is exact: Ctrl+Shift+S is not Ctrl+S.
enum ChordMod : uint8_t {
    MOD_NONE = 0,
    MOD_CTRL = 1 << 0,
    MOD_SHIFT = 1 << 1,
    MOD_ALT = 1 << 2,
    MOD_SUPER = 1 << 3,
};

struct Chord {
    int key = 0;       // an ImGuiKey
    uint8_t mods = 0;  // ChordMod bits

    bool operator==(const Chord& other) const { return key == other.key && mods == other.mods; }
    bool operator!=(const Chord& other) const { return !(*this == other); }
};

// What an action touches, for the perform lock (ADR-051 §4) and external control.
enum class ActionClass { Show, Edit, File, View };

// When its key may fire. Editing: no text field is active and no popup is open.
// Show: only a text field keeps it (F11, Esc, Ctrl+B: the way out must never wait).
enum class KeyGuard { Editing, Show };

// Who invoked the action.
enum class Source { Key, Menu, Control };

struct Action {
    std::string id;
    std::string label;
    std::vector<Chord> chords;          // the first is the one the menu shows
    std::vector<Chord> defaultChords;   // set by add(): what Reset restores
    ActionClass cls = ActionClass::Edit;
    KeyGuard guard = KeyGuard::Editing;
    bool fixed = false;  // never rebindable (F11, Esc, Ctrl+B)
    std::function<bool(const EditorContext&)> enabled;  // empty = always enabled
    std::function<void(EditorContext&)> run;
};

enum class Outcome {
    Ran,
    Disabled,  // enabled() said no, or external control asked for a non-Show action
    Locked,    // the lock policy refused it
    Unknown    // no action has that id
};

enum class AddResult {
    Added,
    Invalid,      // empty id, no run function, or a chord with no key
    DuplicateId,
    ChordClash,   // another action (or this one twice) already has one of its chords
    ReservedKey   // Tab, an arrow or H, with any modifiers: the canvas keys
};

// Why setChords refused, or Changed.
enum class RebindResult {
    Changed,
    UnknownId,
    Fixed,          // F11, Esc, Ctrl+B are never rebound
    Invalid,        // a chord with no key
    ReservedKey,    // the canvas keys, with any modifiers
    NeedsModifier,  // a bare key other than F1-F24 and Delete
    ChordClash      // another action has it (clashWith), or it is listed twice
};

struct RebindCheck {
    RebindResult result = RebindResult::Changed;
    Chord chord;              // the chord refused (not for UnknownId / Fixed)
    std::string clashWith;    // ChordClash: the label of the action that has it
};

// True for the canvas keys, whatever the modifiers.
bool isReservedCanvasKey(int key);
// True for the keys a chord may use without a modifier: F1-F24 and Delete. A bare
// letter or digit would fire while typing outside a text field and take the scene
// take keys 1-9 and the canvas keys' neighbours.
bool bareKeyAllowed(int key);
// True for the keys a chord can name (letters, digits, F1-F24, the navigation and
// punctuation keys, the keypad): not the modifiers themselves, the mouse or a gamepad.
bool isChordKey(int key);

// "Ctrl+Shift+L", "Alt+=", "Delete", "F11": the menu's and the preferences file's form.
// Modifiers in the order Ctrl, Shift, Alt, Super.
std::string chordText(const Chord& chord);
// The chords of an action joined for display: "Ctrl+Y, Ctrl+Shift+Z"; empty for none.
std::string chordsText(const std::vector<Chord>& chords);
// The same chords for showing to the user, with this system's key names: on macOS
// "Cmd+Shift+L" and "Option+=" (editor/KeyLabels.hpp). The file form above never changes.
std::string chordLabel(const Chord& chord);
std::string chordsLabel(const std::vector<Chord>& chords);
// The reverse of chordText: modifiers in any order and case, then one key name.
// nullopt for an unknown key or modifier, or no key.
std::optional<Chord> parseChord(const std::string& text);
// A sentence for a refused rebind, for the Preferences window.
std::string rebindRefusal(const RebindCheck& check);

class ActionRegistry {
public:
    AddResult add(Action action);

    Outcome invoke(const std::string& id, EditorContext& ctx, Source source);

    const Action* find(const std::string& id) const;
    // The action with exactly this chord (key and modifiers) among its chords, or null.
    const Action* findByChord(const Chord& chord) const;
    // Every action, in the order added (the menu's labels and chords).
    const std::deque<Action>& actions() const { return m_actions; }

    // Rebinding. Every chord is checked: the canvas keys, a bare key (bareKeyAllowed),
    // a clash with another action or within the list. Nothing changes unless all pass.
    // An empty list leaves the action on the menu only.
    RebindCheck setChords(const std::string& id, std::vector<Chord> chords);
    // Back to the chords it was added with. False for an unknown or fixed id.
    bool resetChords(const std::string& id);
    void resetAllChords();

    // The perform lock's hook (ADR-051 §4, slice E7): true refuses the action with
    // Outcome::Locked before it runs. Unset = nothing is locked.
    using LockPolicy = std::function<bool(const Action&, Source)>;
    void setLockPolicy(LockPolicy policy) { m_lockPolicy = std::move(policy); }

private:
    Action* findMutable(const std::string& id);

    std::deque<Action> m_actions;  // a deque, so find() pointers survive add()
    LockPolicy m_lockPolicy;
};

// The user's key bindings as the preferences file keeps them: action id -> chords as
// text. Only the actions whose chords differ from their defaults are listed.
using KeyBindings = std::map<std::string, std::vector<std::string>>;

// The registry's rebound actions, for saving.
KeyBindings keyBindingsOf(const ActionRegistry& registry);

// Puts every action back on its default chords, then applies `bindings`. An unknown
// id, a fixed action, an unparseable chord or a chord setChords refuses skips that
// entry (the action keeps its default) with a log line. Overrides are applied after
// every overridden action has let go of its chords, so two actions can swap chords.
// Returns the problems logged, one line each.
std::vector<std::string> applyKeyBindings(ActionRegistry& registry, const KeyBindings& bindings);

}  // namespace quadmapper
