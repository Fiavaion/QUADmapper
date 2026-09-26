#pragma once

// Events that external controllers (OSC over UDP, MIDI over WinMM) hand to the main
// thread (ADR-048). The receive threads only build these and push them into the
// ControlEventQueue; nothing here touches scene state.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace quadmapper::control {

// One OSC argument. i f s T F are decoded; the other OSC 1.0/1.1 types with a known
// size (b h t d c r m S N I) are skipped safely and kept as Unsupported so argument
// positions stay right. A tag outside both sets rejects the whole message.
struct OscArg {
    enum class Type : uint8_t { Int, Float, String, True, False, Unsupported };
    Type type = Type::Unsupported;
    char tag = '\0';     // the type tag character as received
    int32_t i = 0;
    float f = 0.0f;
    std::string s;
};

struct OscMessage {
    std::string address;
    std::vector<OscArg> args;
};

enum class MidiType : uint8_t { NoteOn, NoteOff, ControlChange, ProgramChange };

// A decoded MIDI channel message. NoteOn always has velocity > 0: a note-on with
// velocity 0 is decoded as NoteOff (the usual running-status note-off idiom).
struct MidiMessage {
    MidiType type = MidiType::NoteOn;
    uint8_t channel = 1;  // 1-16
    uint8_t number = 0;   // note / controller / program, 0-127
    uint8_t value = 0;    // velocity / controller value, 0-127 (0 for program change)
};

// Fixed-size device name so a MIDI event carries no heap memory: the WinMM callback
// fills one without allocating. WinMM names are at most 31 UTF-16 units, which is
// at most 93 UTF-8 bytes.
using DeviceName = std::array<char, 96>;

struct ControlEvent {
    enum class Source : uint8_t { Osc, Midi };
    Source source = Source::Osc;
    OscMessage osc;           // Source::Osc
    uint32_t oscSenderIp = 0;    // Source::Osc: sender's IPv4 address, host byte order
    uint16_t oscSenderPort = 0;  // Source::Osc: sender's UDP port
    MidiMessage midi;         // Source::Midi
    DeviceName device{};      // Source::Midi: NUL-terminated input device name
};

}  // namespace quadmapper::control
