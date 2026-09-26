#include "MidiDecode.hpp"

namespace quadmapper::control {

std::optional<MidiMessage> decodeMidiShortMessage(uint32_t message) {
    const uint8_t status = static_cast<uint8_t>(message & 0xFF);
    if ((status & 0x80) == 0) return std::nullopt;  // no status byte (see header)

    MidiMessage m;
    m.channel = static_cast<uint8_t>((status & 0x0F) + 1);
    m.number = static_cast<uint8_t>((message >> 8) & 0x7F);
    const uint8_t data2 = static_cast<uint8_t>((message >> 16) & 0x7F);

    switch (status & 0xF0) {
        case 0x80:
            m.type = MidiType::NoteOff;
            m.value = data2;
            return m;
        case 0x90:
            m.type = data2 > 0 ? MidiType::NoteOn : MidiType::NoteOff;
            m.value = data2;
            return m;
        case 0xB0:
            m.type = MidiType::ControlChange;
            m.value = data2;
            return m;
        case 0xC0:
            m.type = MidiType::ProgramChange;
            m.value = 0;
            return m;
        default:
            return std::nullopt;
    }
}

}  // namespace quadmapper::control
