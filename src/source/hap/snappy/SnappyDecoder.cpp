// Minimal Snappy decompressor (decode only).
// Snappy block format: https://github.com/google/snappy/blob/main/format_description.txt
// © Google Inc. (BSD-3-Clause); this is a clean-room decode-only implementation.
// See THIRD-PARTY-NOTICES.md.

#include "source/hap/snappy/SnappyDecoder.hpp"

#include <cstring>
#include <limits>

namespace quadmapper {
namespace hap {
namespace snappy {

// Hard cap: reject any block claiming > 64 MiB uncompressed. HAP video frames
// at 4K are well under 32 MiB; 64 MiB is generous yet blocks OOM from crafted files.
static constexpr size_t MAX_SNAPPY_UNCOMPRESSED = 64u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// Varint decoder (little-endian, 7 bits/byte, MSB = continuation)
// ---------------------------------------------------------------------------
static bool readVarint32(const uint8_t* data, size_t size, size_t& pos, uint32_t& value) {
    value = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (pos >= size) return false;
        uint8_t b = data[pos++];
        value |= static_cast<uint32_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return true;
    }
    return false; // varint too long
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
bool decompressSnappy(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out) {
    if (!src || srcLen == 0) return false;

    size_t pos = 0;

    // 1. Read uncompressed length (varint32).
    uint32_t uncompressedLen = 0;
    if (!readVarint32(src, srcLen, pos, uncompressedLen)) return false;
    if (uncompressedLen > MAX_SNAPPY_UNCOMPRESSED) return false;

    out.clear();
    out.resize(uncompressedLen);
    size_t outPos = 0;

    // 2. Decode elements until input is exhausted.
    while (pos < srcLen) {
        uint8_t tag = src[pos++];
        uint8_t type = tag & 0x03;

        if (type == 0x00) {
            // ---------------------------------------------------------------
            // Literal element.
            // Bits [7:2] of the tag encode length-1 (0..59) directly.
            // Values 60-63 mean the length is in the following 1-4 bytes.
            // ---------------------------------------------------------------
            uint32_t literalLen;
            uint8_t lenBits = (tag >> 2) & 0x3F;
            if (lenBits < 60) {
                literalLen = static_cast<uint32_t>(lenBits) + 1;
            } else {
                uint8_t extraBytes = lenBits - 59; // 1..4
                if (pos + extraBytes > srcLen) return false;
                literalLen = 0;
                for (uint8_t i = 0; i < extraBytes; ++i) {
                    literalLen |= static_cast<uint32_t>(src[pos++]) << (i * 8);
                }
                literalLen += 1;
            }
            if (pos + literalLen > srcLen) return false;
            if (outPos + literalLen > uncompressedLen) return false;
            std::memcpy(out.data() + outPos, src + pos, literalLen);
            outPos += literalLen;
            pos    += literalLen;

        } else if (type == 0x01) {
            // ---------------------------------------------------------------
            // Copy with 1-byte offset.
            // Bits [5:3] = length-4 (0..7 → 4..11)
            // Bits [7:6] combined with next byte = 11-bit offset
            // ---------------------------------------------------------------
            if (pos + 1 > srcLen) return false;
            uint32_t copyLen    = static_cast<uint32_t>((tag >> 2) & 0x07) + 4;
            uint32_t offset     = ((static_cast<uint32_t>(tag) & 0xE0) << 3)
                                | static_cast<uint32_t>(src[pos++]);
            if (offset == 0 || outPos < offset) return false;
            if (outPos + copyLen > uncompressedLen) return false;
            // Byte-by-byte to handle overlapping runs correctly.
            const uint8_t* copyFrom = out.data() + outPos - offset;
            uint8_t*       copyTo   = out.data() + outPos;
            for (uint32_t i = 0; i < copyLen; ++i) copyTo[i] = copyFrom[i];
            outPos += copyLen;

        } else if (type == 0x02) {
            // ---------------------------------------------------------------
            // Copy with 2-byte offset.
            // Bits [7:2] of tag = length-1 (0..62 → 1..63)
            // Following 2 bytes = 16-bit little-endian offset
            // ---------------------------------------------------------------
            if (pos + 2 > srcLen) return false;
            uint32_t copyLen = static_cast<uint32_t>((tag >> 2) & 0x3F) + 1;
            uint32_t offset  = static_cast<uint32_t>(src[pos])
                             | (static_cast<uint32_t>(src[pos + 1]) << 8);
            pos += 2;
            if (offset == 0 || outPos < offset) return false;
            if (outPos + copyLen > uncompressedLen) return false;
            const uint8_t* copyFrom = out.data() + outPos - offset;
            uint8_t*       copyTo   = out.data() + outPos;
            for (uint32_t i = 0; i < copyLen; ++i) copyTo[i] = copyFrom[i];
            outPos += copyLen;

        } else { // type == 0x03
            // ---------------------------------------------------------------
            // Copy with 4-byte offset.
            // Bits [7:2] of tag = length-1
            // Following 4 bytes = 32-bit little-endian offset
            // ---------------------------------------------------------------
            if (pos + 4 > srcLen) return false;
            uint32_t copyLen = static_cast<uint32_t>((tag >> 2) & 0x3F) + 1;
            uint32_t offset  = static_cast<uint32_t>(src[pos])
                             | (static_cast<uint32_t>(src[pos + 1]) << 8)
                             | (static_cast<uint32_t>(src[pos + 2]) << 16)
                             | (static_cast<uint32_t>(src[pos + 3]) << 24);
            pos += 4;
            if (offset == 0 || outPos < offset) return false;
            if (outPos + copyLen > uncompressedLen) return false;
            const uint8_t* copyFrom = out.data() + outPos - offset;
            uint8_t*       copyTo   = out.data() + outPos;
            for (uint32_t i = 0; i < copyLen; ++i) copyTo[i] = copyFrom[i];
            outPos += copyLen;
        }
    }

    // 3. Validate we wrote exactly as many bytes as promised.
    if (outPos != uncompressedLen) {
        out.clear();
        return false;
    }
    return true;
}

} // namespace snappy
} // namespace hap
} // namespace quadmapper
