#pragma once

// The action registry (ADR-051 §3): every editor-wide command with a key or a menu
// item is one Action, and the keys (ui/ShortcutRouter), the menu and external control
// all invoke it by id, so they cannot disagree. The canvas keys (Tab / Shift+Tab, the
// arrows with Shift 10x and Alt 0.1x, H / Shift+H) stay outside it: they are the
// success metric (input/VertexKeyboard.hpp), and add() refuses any chord on them.

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>

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
    std::optional<Chord> chord;
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
    ChordClash,   // another action already has this exact chord
    ReservedKey   // Tab, an arrow or H, with any modifiers: the canvas keys
};

// True for the canvas keys, whatever the modifiers.
bool isReservedCanvasKey(int key);

class ActionRegistry {
public:
    AddResult add(Action action);

    Outcome invoke(const std::string& id, EditorContext& ctx, Source source);

    const Action* find(const std::string& id) const;
    // The action on exactly this chord (key and modifiers), or null.
    const Action* findByChord(const Chord& chord) const;
    // Every action, in the order added (the menu's labels and chords).
    const std::deque<Action>& actions() const { return m_actions; }

    // The perform lock's hook (ADR-051 §4, slice E7): true refuses the action with
    // Outcome::Locked before it runs. Unset = nothing is locked.
    using LockPolicy = std::function<bool(const Action&, Source)>;
    void setLockPolicy(LockPolicy policy) { m_lockPolicy = std::move(policy); }

private:
    std::deque<Action> m_actions;  // a deque, so find() pointers survive add()
    LockPolicy m_lockPolicy;
};

}  // namespace quadmapper
