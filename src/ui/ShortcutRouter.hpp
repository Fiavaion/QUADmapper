#pragma once

// The editor-wide keys (ADR-051 §3): once a frame, each registered action whose chord
// was pressed this frame, with exactly its modifiers, is invoked if its KeyGuard lets
// it. The router only asks ImGui about registered chords, and the registry refuses
// the canvas keys (Tab, the arrows, H), so it never reads or consumes them: those stay
// with ViewportEditor and input/VertexKeyboard.hpp, the success metric.

#include "editor/ActionRegistry.hpp"

#include <string>
#include <vector>

namespace quadmapper {

struct EditorContext;

// The modifiers held now, as ChordMod bits.
uint8_t currentChordMods();

// Whether a key with this guard may act now. Editing: no text field is active and no
// popup is open. Show: no text field is active.
bool keyGuardAllows(KeyGuard guard);

// The router. Call once per frame, after the canvas has read its keys. An action fires
// on any of its chords. Nothing fires while EditorState::capturingChord is set (the
// Keyboard preferences take that key). Returns the ids of the actions invoked this
// frame (whatever their Outcome). The menu's chord text is chordText (ActionRegistry).
std::vector<std::string> routeShortcuts(ActionRegistry& registry, EditorContext& ctx);

// The scene take keys 1-9 pressed since the last frame (physical keys, queued outside
// ImGui, US-008), then cleared. Each is invoked (Source::Key) unless a text field is
// active (the digit was typed there), a chord is being captured, or a popup is open. The "Unlock the Show?" prompt
// lets them through: the show stays live while it asks. A take a popup holds off is
// not dropped silently: the status line says so.
void runSceneTakeKeys(ActionRegistry& registry, EditorContext& ctx, std::vector<int>& numbers);

}  // namespace quadmapper
