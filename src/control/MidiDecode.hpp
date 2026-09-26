#pragma once

#include "ControlEvent.hpp"

#include <cstdint>
#include <optional>

namespace quadmapper::control {

// Decode a WinMM MIM_DATA short message (dwParam1): status in the low byte, then
// data1, then data2. WinMM expands running status before delivery (MIM_DATA: "MIDI
// messages received from a MIDI input port have running status disabled; each
// message is expanded to include the MIDI status byte"), so a low byte without the
// status bit set is malformed and rejected here rather than guessed at.
//
// Decoded: note off (0x8n), note on (0x9n; velocity 0 = note off), control change
// (0xBn) and program change (0xCn). Everything else (aftertouch, pitch bend, system
// and real-time messages, SysEx) returns nullopt. Data bytes are masked to 7 bits.
std::optional<MidiMessage> decodeMidiShortMessage(uint32_t message);

}  // namespace quadmapper::control
