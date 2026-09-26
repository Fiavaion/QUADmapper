#pragma once

#include <optional>
#include <string>

namespace quadmapper::control {

// What a MIDI mapping listens to.
enum class MidiControlType { Note, CC, Program };

// How a mapped control drives its target (ADR-048 §4). "Press" is a note-on with
// velocity > 0, a CC crossing 64 upward, or a program change; "release" is a
// note-off or a CC crossing 64 downward.
//   Trigger   - on press: a Trigger target fires, a Bool target toggles, a Float
//               target goes to its maximum.
//   Toggle    - on press: a Bool (or Float) target flips between off/min and on/max;
//               a Trigger target fires.
//   Momentary - on while held: press sets on/max, release sets off/min; a Trigger
//               target fires on press.
//   Value     - the control's value drives the target: CC 0-127 (or note velocity,
//               note-off = 0) maps linearly into a Float target's range, >= 64 is on
//               for a Bool target; a Trigger target fires on press. 7-bit only.
enum class MidiMode { Trigger, Toggle, Momentary, Value };

// One learned binding, saved with the show (.qmap "control": {"midi": [...]}).
struct MidiMapping {
    std::string device = "any";     // input device name, or "any"
    MidiControlType type = MidiControlType::Note;
    int channel = 0;                // 1-16, 0 = any
    int number = 0;                 // note / controller / program, 0-127
    std::string target;             // control path, e.g. /qm/quad/<uuid>/opacity
    MidiMode mode = MidiMode::Trigger;

    // Runtime only (never saved, not part of equality).
    bool ccHigh = false;            // last CC value was >= 64 (edge detection)
    double lastActivity = -1.0;     // seconds on the caller's clock; < 0 = never
    int lastValue = -1;             // last 7-bit value seen, -1 = none
};

inline bool sameBinding(const MidiMapping& a, const MidiMapping& b) {
    return a.device == b.device && a.type == b.type && a.channel == b.channel &&
           a.number == b.number && a.target == b.target && a.mode == b.mode;
}

inline const char* toString(MidiControlType type) {
    switch (type) {
        case MidiControlType::Note:    return "note";
        case MidiControlType::CC:      return "cc";
        case MidiControlType::Program: return "program";
    }
    return "note";
}

inline const char* toString(MidiMode mode) {
    switch (mode) {
        case MidiMode::Trigger:   return "trigger";
        case MidiMode::Toggle:    return "toggle";
        case MidiMode::Momentary: return "momentary";
        case MidiMode::Value:     return "value";
    }
    return "trigger";
}

inline std::optional<MidiControlType> parseMidiControlType(const std::string& text) {
    if (text == "note") return MidiControlType::Note;
    if (text == "cc") return MidiControlType::CC;
    if (text == "program") return MidiControlType::Program;
    return std::nullopt;
}

inline std::optional<MidiMode> parseMidiMode(const std::string& text) {
    if (text == "trigger") return MidiMode::Trigger;
    if (text == "toggle") return MidiMode::Toggle;
    if (text == "momentary") return MidiMode::Momentary;
    if (text == "value") return MidiMode::Value;
    return std::nullopt;
}

}  // namespace quadmapper::control
