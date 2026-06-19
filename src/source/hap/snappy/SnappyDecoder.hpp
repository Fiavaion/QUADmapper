#pragma once

// Minimal Snappy decompressor — decode only, no compression.
// Implements the Snappy block format as documented at:
//   https://github.com/google/snappy/blob/main/format_description.txt
//   (Snappy format © Google Inc., originally released under BSD-3-Clause.)
//
// This is a clean-room decode-only implementation in ~200 lines.
// Listed in THIRD-PARTY-NOTICES.md.

#include <cstdint>
#include <cstddef>
#include <vector>

namespace quadmapper {
namespace hap {
namespace snappy {

/// Decompress a Snappy-encoded block (framing-free — raw block format, as used
/// by HAP). The uncompressed length is encoded in a varint at the start of the
/// block. Returns true and fills `out` on success; returns false (leaving `out`
/// in an unspecified state) on any malformed or truncated input.
///
/// Security: out is pre-reserved to the declared uncompressed length; that
/// length is validated against a hard cap (MAX_SNAPPY_UNCOMPRESSED) before
/// allocation.
bool decompressSnappy(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out);

} // namespace snappy
} // namespace hap
} // namespace quadmapper
