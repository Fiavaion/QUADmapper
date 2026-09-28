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
//               for a Bool target; a Trigger target fires on press. A 14-bit CC
//               uses its 0-16383 value (on = >= 8192).
enum class MidiMode { Trigger, Toggle, Momentary, Value };

// 14-bit CC (MIDI 1.0 "high resolution" controllers): controller n (0-31) carries
// the value's upper 7 bits (MSB) and n + 32 the lower 7 (LSB). A 14-bit mapping's
// `number` is the MSB controller and it listens to both.
//   - MSB: kept; the LSB resets to 0 (MIDI 1.0) unless an LSB arrived since the
//     previous MSB within k14BitPairSeconds (a controller that sends the LSB first);
//     the value is applied, and press/release edges come from the MSB alone.
//   - LSB: kept and the value applied, once any MSB has been seen.
// Both halves of a move normally arrive in the same frame, so only the final value
// is ever rendered. NRPN/RPN are not decoded.
// Seconds on the dispatcher's clock (frame time, so a pair split across a frame or
// two still counts).
constexpr double k14BitPairSeconds = 0.1;
constexpr int k14BitMaxMsbNumber = 31;
constexpr int k14BitLsbOffset = 32;

// One learned binding, saved with the show (.qmap "control": {"midi": [...]}).
struct MidiMapping {
    std::string device = "any";     // input device name, or "any"
    MidiControlType type = MidiControlType::Note;
    int channel = 0;                // 1-16, 0 = any
    int number = 0;                 // note / controller / program, 0-127
    std::string target;             // control path, e.g. /qm/quad/<uuid>/opacity
    MidiMode mode = MidiMode::Trigger;
    int bits = 7;                   // 14 = a 14-bit CC pair (type CC, number 0-31)

    // Runtime only (never saved, not part of equality).
    bool ccHigh = false;            // last CC value (MSB) was >= 64 (edge detection)
    double lastActivity = -1.0;     // seconds on the caller's clock; < 0 = never
    int lastValue = -1;             // last value seen (7- or 14-bit), -1 = none
    int msb = -1;                   // 14-bit: last MSB, -1 = none yet
    int lsb = 0;                    // 14-bit: last LSB
    bool lsbSinceMsb = false;       // 14-bit: an LSB arrived after the last MSB
    double lsbTime = -1.0;          // 14-bit: when that LSB arrived

    bool is14Bit() const { return bits == 14 && type == MidiControlType::CC; }
};

inline bool sameBinding(const MidiMapping& a, const MidiMapping& b) {
    return a.device == b.device && a.type == b.type && a.channel == b.channel &&
           a.number == b.number && a.target == b.target && a.mode == b.mode && a.bits == b.bits;
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
