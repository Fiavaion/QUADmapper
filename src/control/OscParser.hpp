#pragma once

#include "ControlEvent.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace quadmapper::control {

// Hand-written OSC 1.0 packet parser (ADR-048 §1). Packets come from the network,
// so every length is checked against the bytes actually present and nothing is
// trusted:
//   - a packet (and every bundle element) is a non-zero multiple of 4 bytes;
//   - a message is an address string starting with '/', then an optional type-tag
//     string starting with ','; a message with no type-tag string has no arguments;
//   - strings must be NUL-terminated inside the data and padded to 4 bytes;
//   - arguments i f s T F are decoded; b h t d c r m S N I (and the [ ] array
//     markers, which carry no data) are skipped by their known sizes, and any other
//     tag rejects the packet, since its size cannot be known;
//   - bundles ("#bundle", time tag, then size-prefixed elements) may nest up to
//     kMaxBundleDepth; their time tags are ignored and everything runs on receipt;
//   - at most kMaxMessagesPerPacket messages are accepted from one packet.
// Address patterns (* ? [] {}) are not expanded: an address is matched literally.
constexpr int kMaxBundleDepth = 8;
constexpr size_t kMaxMessagesPerPacket = 1024;

// Parse one UDP datagram. On success appends every message it holds to `out` and
// returns true. On any malformation returns false and leaves `out` unchanged.
bool parseOscPacket(const uint8_t* data, size_t size, std::vector<OscMessage>& out);

}  // namespace quadmapper::control
