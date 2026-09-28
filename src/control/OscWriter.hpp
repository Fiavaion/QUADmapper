#pragma once

#include "ControlEvent.hpp"

#include <cstdint>
#include <vector>

namespace quadmapper::control {

// OSC 1.0 message writer, the inverse of parseOscPacket for the types it decodes:
// the address string, the type-tag string, then each argument big-endian and every
// string NUL-terminated and padded to 4 bytes. Arguments: i f s T F.
//
// Returns false, leaving `out` unchanged, when the message cannot be written
// faithfully: an address that is empty or does not start with '/', a string (or the
// address) holding a NUL, or an Unsupported argument. On success appends the packet
// to `out`.
bool writeOscMessage(const OscMessage& message, std::vector<uint8_t>& out);

}  // namespace quadmapper::control
