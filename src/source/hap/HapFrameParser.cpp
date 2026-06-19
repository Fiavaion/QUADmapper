// HAP frame parser — pure CPU, no GL, no FFmpeg.
// Spec: https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md

#include "source/hap/HapFrameParser.hpp"
#include "source/hap/snappy/SnappyDecoder.hpp"

#include <cstring>
#include <limits>

namespace quadmapper {
namespace hap {

// ---------------------------------------------------------------------------
// Internal constants — taken directly from the spec table
// ---------------------------------------------------------------------------

// High-nibble of section-type byte: second-stage compressor
static constexpr uint8_t kCompressorNone     = 0xA0; // No second-stage (raw BCn)
static constexpr uint8_t kCompressorSnappy   = 0xB0; // Snappy
static constexpr uint8_t kCompressorComplex  = 0xC0; // Decode Instructions Container

// Low-nibble of section-type byte: texture format
static constexpr uint8_t kFormatDXT1        = 0x0B; // RGB BC1  (Hap)
static constexpr uint8_t kFormatDXT5RGBA    = 0x0E; // RGBA BC3 (Hap Alpha)
static constexpr uint8_t kFormatDXT5YCoCg   = 0x0F; // YCoCg BC3 (Hap Q)
static constexpr uint8_t kFormatBC7         = 0x0C; // RGBA BC7 (Hap R)
static constexpr uint8_t kFormatRGTC1       = 0x01; // Alpha BC4 (Hap Q Alpha plane)

// Decode Instructions Container subsection types
static constexpr uint8_t kSectionDecodeInstructions = 0x01;
static constexpr uint8_t kSectionChunkCompressors   = 0x02;
static constexpr uint8_t kSectionChunkSizes         = 0x03;
static constexpr uint8_t kSectionChunkOffsets       = 0x04; // optional

// Chunk compressor IDs (from the spec, compressor table byte values)
static constexpr uint8_t kChunkCompressorNone   = 0x0A;
static constexpr uint8_t kChunkCompressorSnappy = 0x0B;

// Multi-image section-type (Hap Q Alpha = primary YCoCg + secondary BC4)
static constexpr uint8_t kSectionMultiImage = 0x0D;

// Hard cap: reject any single section claiming > 128 MiB of texture data.
// 4K BC3 = 8 bpp × 3840 × 2160 ≈ 31.6 MiB; 128 MiB is generous yet bounded.
static constexpr size_t MAX_SECTION_SIZE = 128u * 1024u * 1024u;

// Hard cap on chunk count to prevent degenerate allocation in the chunk tables.
static constexpr uint32_t MAX_CHUNK_COUNT = 65536u;

// ---------------------------------------------------------------------------
// Buffer reader helper — tracks position and enforces bounds
// ---------------------------------------------------------------------------
struct Reader {
    const uint8_t* data;
    size_t         size;
    size_t         pos = 0;

    bool hasBytes(size_t n) const { return (size - pos) >= n; }
    bool atEnd()            const { return pos >= size; }

    bool readU8(uint8_t& v) {
        if (!hasBytes(1)) return false;
        v = data[pos++];
        return true;
    }
    bool readU24LE(uint32_t& v) {
        if (!hasBytes(3)) return false;
        v = static_cast<uint32_t>(data[pos])
          | (static_cast<uint32_t>(data[pos + 1]) << 8)
          | (static_cast<uint32_t>(data[pos + 2]) << 16);
        pos += 3;
        return true;
    }
    bool readU32LE(uint32_t& v) {
        if (!hasBytes(4)) return false;
        v = static_cast<uint32_t>(data[pos])
          | (static_cast<uint32_t>(data[pos + 1]) << 8)
          | (static_cast<uint32_t>(data[pos + 2]) << 16)
          | (static_cast<uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return true;
    }

    // Advance pos without reading; returns false if out of range.
    bool skip(size_t n) {
        if (!hasBytes(n)) return false;
        pos += n;
        return true;
    }
};

// ---------------------------------------------------------------------------
// Read a section header. Returns false on malformed/truncated input.
// sectionType: raw section-type byte (caller decodes nibbles).
// sectionSize: payload size in bytes EXCLUDING the header itself.
// ---------------------------------------------------------------------------
static bool readSectionHeader(Reader& r, uint8_t& sectionType, uint32_t& sectionSize) {
    // First, try the 4-byte header form:
    //   bytes 0-2: 24-bit LE size  (may be 0 → use 8-byte form)
    //   byte  3:   section type
    uint32_t size24 = 0;
    if (!r.readU24LE(size24)) return false;

    uint8_t type = 0;
    if (!r.readU8(type)) return false;

    if (size24 == 0) {
        // Extended 8-byte header: next 4 bytes are the actual 32-bit size.
        uint32_t size32 = 0;
        if (!r.readU32LE(size32)) return false;
        sectionSize = size32;
    } else {
        sectionSize = size24;
    }

    sectionType = type;
    return true;
}

// ---------------------------------------------------------------------------
// Map low-nibble of section-type byte → HapTextureFormat
// ---------------------------------------------------------------------------
static HapTextureFormat mapTextureFormat(uint8_t lowNibble) {
    switch (lowNibble) {
        case kFormatDXT1:     return HapTextureFormat::RGB_DXT1;
        case kFormatDXT5RGBA: return HapTextureFormat::RGBA_DXT5;
        case kFormatDXT5YCoCg:return HapTextureFormat::YCoCg_DXT5;
        case kFormatBC7:      return HapTextureFormat::RGBA_BPTC;
        case kFormatRGTC1:    return HapTextureFormat::Alpha_RGTC1;
        default:              return HapTextureFormat::None;
    }
}

// ---------------------------------------------------------------------------
// Decompress a Snappy block → append to dest.
// Returns false on any failure.
// ---------------------------------------------------------------------------
static bool appendSnappy(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& dest) {
    std::vector<uint8_t> tmp;
    if (!snappy::decompressSnappy(src, srcLen, tmp)) return false;
    dest.insert(dest.end(), tmp.begin(), tmp.end());
    return true;
}

// ---------------------------------------------------------------------------
// Decode a Complex (chunked) payload.
//
// The payload is a Decode Instructions Container (type 0x01) followed
// immediately by the concatenated chunk data.
//
// Container layout (spec §Decode Instructions Container):
//   4- or 8-byte header (type 0x01)
//     subsection type 0x02 — chunk compressor table (1 byte/chunk)
//     subsection type 0x03 — chunk size table       (4 bytes/chunk, LE)
//     subsection type 0x04 — chunk offset table     (optional, 4 bytes/chunk, LE)
//
// After the container, the chunk data immediately follows.
// ---------------------------------------------------------------------------
static bool decodeComplex(const uint8_t* payload, size_t payloadLen,
                          std::vector<uint8_t>& out) {
    if (payloadLen == 0) return false;
    Reader r{payload, payloadLen};

    // 1. Read the Decode Instructions Container header.
    uint8_t  containerType = 0;
    uint32_t containerSize = 0;
    if (!readSectionHeader(r, containerType, containerSize)) return false;
    if (containerType != kSectionDecodeInstructions) return false;

    // The container's declared size covers its subsections.
    size_t containerEnd = r.pos + containerSize;
    if (containerEnd > payloadLen) return false; // container claims too much

    // 2. Parse subsections inside the container.
    std::vector<uint8_t>  chunkCompressors;
    std::vector<uint32_t> chunkSizes;
    std::vector<uint32_t> chunkOffsets; // optional

    while (r.pos < containerEnd) {
        uint8_t  subType = 0;
        uint32_t subSize = 0;
        if (!readSectionHeader(r, subType, subSize)) return false;
        if (r.pos + subSize > containerEnd) return false;

        if (subType == kSectionChunkCompressors) {
            // 1 byte per chunk
            if (subSize > MAX_CHUNK_COUNT) return false;
            chunkCompressors.resize(subSize);
            std::memcpy(chunkCompressors.data(), r.data + r.pos, subSize);
        } else if (subType == kSectionChunkSizes) {
            // 4 bytes per chunk, LE uint32
            if ((subSize % 4) != 0) return false;
            uint32_t count = subSize / 4;
            if (count > MAX_CHUNK_COUNT) return false;
            chunkSizes.resize(count);
            Reader sr{r.data + r.pos, subSize};
            for (uint32_t i = 0; i < count; ++i) {
                if (!sr.readU32LE(chunkSizes[i])) return false;
            }
        } else if (subType == kSectionChunkOffsets) {
            // 4 bytes per chunk, LE uint32 (optional)
            if ((subSize % 4) != 0) return false;
            uint32_t count = subSize / 4;
            if (count > MAX_CHUNK_COUNT) return false;
            chunkOffsets.resize(count);
            Reader sr{r.data + r.pos, subSize};
            for (uint32_t i = 0; i < count; ++i) {
                if (!sr.readU32LE(chunkOffsets[i])) return false;
            }
        }
        // Skip any unrecognised subsection type.
        r.pos += subSize;
    }

    // 3. Validate we have both required tables.
    if (chunkCompressors.empty() || chunkSizes.empty()) return false;
    if (chunkCompressors.size() != chunkSizes.size()) return false;
    if (!chunkOffsets.empty() && chunkOffsets.size() != chunkSizes.size()) return false;

    uint32_t chunkCount = static_cast<uint32_t>(chunkSizes.size());

    // 4. Build chunk offsets if the optional table was absent (cumulative).
    if (chunkOffsets.empty()) {
        chunkOffsets.resize(chunkCount);
        uint32_t cursor = 0;
        for (uint32_t i = 0; i < chunkCount; ++i) {
            chunkOffsets[i] = cursor;
            // Overflow-safe addition: reject if chunk would push past uint32 max.
            if (chunkSizes[i] > std::numeric_limits<uint32_t>::max() - cursor) return false;
            cursor += chunkSizes[i];
        }
    }

    // 5. Chunk data starts right after the container (r.pos after skipping containerSize).
    // r.pos is already at containerEnd after the subsection loop.
    size_t chunkDataBase = containerEnd; // absolute offset within payload[]

    // Validate total chunk data fits in the payload.
    for (uint32_t i = 0; i < chunkCount; ++i) {
        size_t chunkStart = chunkDataBase + chunkOffsets[i];
        size_t chunkEnd   = chunkStart + chunkSizes[i];
        if (chunkEnd < chunkStart) return false; // overflow
        if (chunkEnd > payloadLen) return false;  // out of bounds
    }

    // 6. Decompress / copy each chunk in order → append to out.
    for (uint32_t i = 0; i < chunkCount; ++i) {
        size_t        chunkStart = chunkDataBase + chunkOffsets[i];
        const uint8_t* chunk     = payload + chunkStart;
        size_t         chunkLen  = chunkSizes[i];

        if (chunkCompressors[i] == kChunkCompressorSnappy) {
            if (!appendSnappy(chunk, chunkLen, out)) return false;
        } else if (chunkCompressors[i] == kChunkCompressorNone) {
            out.insert(out.end(), chunk, chunk + chunkLen);
        } else {
            return false; // unknown chunk compressor
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Decode a single top-level section into texture bytes.
// Returns false on any error.
// ---------------------------------------------------------------------------
static bool decodeSingleSection(const uint8_t* data, size_t size,
                                uint8_t sectionType,
                                std::vector<uint8_t>& textureBytes) {
    uint8_t compressor = sectionType & 0xF0;
    // payload starts at data[0], length = size (already resolved by caller)

    if (compressor == kCompressorNone) {
        // Raw BCn bytes — direct copy.
        if (size > MAX_SECTION_SIZE) return false;
        textureBytes.insert(textureBytes.end(), data, data + size);
        return true;

    } else if (compressor == kCompressorSnappy) {
        if (size > MAX_SECTION_SIZE) return false;
        return appendSnappy(data, size, textureBytes);

    } else if (compressor == kCompressorComplex) {
        return decodeComplex(data, size, textureBytes);
    }

    return false; // unknown compressor
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
HapDecodedFrame parseHapFrame(const uint8_t* data, size_t size) {
    HapDecodedFrame result;
    if (!data || size < 4) return result; // need at least a 4-byte header

    Reader r{data, size};

    uint8_t  topType = 0;
    uint32_t topSize = 0;
    if (!readSectionHeader(r, topType, topSize)) return result;

    // Validate the declared section size fits in the remaining buffer.
    if (topSize > (size - r.pos)) return result;

    const uint8_t* payload    = data + r.pos;
    size_t         payloadLen = topSize;

    // -----------------------------------------------------------------------
    // Multi-image section (0x0D) — Hap Q Alpha: primary + secondary planes.
    // The payload is itself a sequence of section headers.
    // -----------------------------------------------------------------------
    if (topType == kSectionMultiImage) {
        Reader mr{payload, payloadLen};
        bool parsedPrimary   = false;
        bool parsedSecondary = false;

        while (!mr.atEnd()) {
            uint8_t  subType = 0;
            uint32_t subSize = 0;
            if (!readSectionHeader(mr, subType, subSize)) return result;
            if (subSize > (payloadLen - mr.pos)) return result;

            uint8_t lowNibble  = subType & 0x0F;
            HapTextureFormat fmt = mapTextureFormat(lowNibble);
            if (fmt == HapTextureFormat::None) {
                mr.pos += subSize;
                continue; // skip unknown sub-sections gracefully
            }

            std::vector<uint8_t> subBytes;
            if (!decodeSingleSection(mr.data + mr.pos, subSize, subType, subBytes)) {
                return result;
            }

            if (!parsedPrimary && fmt != HapTextureFormat::Alpha_RGTC1) {
                result.format      = fmt;
                result.textureData = std::move(subBytes);
                parsedPrimary      = true;
            } else if (!parsedSecondary && fmt == HapTextureFormat::Alpha_RGTC1) {
                result.alphaPlane  = std::move(subBytes);
                parsedSecondary    = true;
            }

            mr.pos += subSize;
        }
        return result; // valid() if parsedPrimary
    }

    // -----------------------------------------------------------------------
    // Single top-level section — normal Hap, Hap Alpha, Hap Q, Hap R.
    // -----------------------------------------------------------------------
    uint8_t lowNibble = topType & 0x0F;
    HapTextureFormat fmt = mapTextureFormat(lowNibble);
    if (fmt == HapTextureFormat::None) return result; // unsupported format

    std::vector<uint8_t> textureBytes;
    if (!decodeSingleSection(payload, payloadLen, topType, textureBytes)) return result;

    result.format      = fmt;
    result.textureData = std::move(textureBytes);
    return result;
}

} // namespace hap
} // namespace quadmapper
