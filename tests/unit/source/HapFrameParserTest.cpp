#include <gtest/gtest.h>
#include "source/hap/HapFrameParser.hpp"
#include "source/hap/snappy/SnappyDecoder.hpp"

#include <vector>
#include <cstdint>

using namespace quadmapper::hap;
using quadmapper::hap::snappy::decompressSnappy;

// ---------------------------------------------------------------------------
// Helper: build a trivial all-literal Snappy block for a given payload.
//
// Snappy block format:
//   varint32(uncompressedLen)
//   then for each ≤60-byte run: tag byte 0x00|((len-1)<<2) + raw bytes
//
// This covers literal chunks up to 60 bytes per element (sufficient for tests).
// ---------------------------------------------------------------------------
static std::vector<uint8_t> makeSnappyLiterals(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;

    // Write varint32 for uncompressed length (simple ≤127-byte case)
    uint32_t totalLen = static_cast<uint32_t>(payload.size());
    // Varint encode: 7 bits per byte, MSB = continuation
    uint32_t v = totalLen;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);

    // Emit payload as literal elements, ≤60 bytes each.
    size_t offset = 0;
    while (offset < payload.size()) {
        size_t chunkLen = std::min<size_t>(payload.size() - offset, 60);
        // tag: type=0 (literal), lenBits = chunkLen-1 (for ≤60 bytes, fits in [7:2])
        out.push_back(static_cast<uint8_t>(((chunkLen - 1) & 0x3F) << 2));
        out.insert(out.end(), payload.begin() + offset, payload.begin() + offset + chunkLen);
        offset += chunkLen;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Helper: build a 4-byte or 8-byte section header.
// sectionSize = payload bytes (NOT including the header itself).
// ---------------------------------------------------------------------------
static void appendHeader(std::vector<uint8_t>& buf, uint8_t sectionType, uint32_t sectionSize) {
    if (sectionSize <= 0x00FFFFFFu) {
        // 4-byte header: 24-bit size + type byte
        buf.push_back(static_cast<uint8_t>(sectionSize & 0xFF));
        buf.push_back(static_cast<uint8_t>((sectionSize >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>((sectionSize >> 16) & 0xFF));
        buf.push_back(sectionType);
    } else {
        // 8-byte extended header: 3 zero bytes + type + 32-bit size
        buf.push_back(0x00);
        buf.push_back(0x00);
        buf.push_back(0x00);
        buf.push_back(sectionType);
        buf.push_back(static_cast<uint8_t>(sectionSize & 0xFF));
        buf.push_back(static_cast<uint8_t>((sectionSize >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>((sectionSize >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((sectionSize >> 24) & 0xFF));
    }
}

// A tiny "BC1 block" payload — 8 bytes (one 4×4 BC1 block) of recognisable data.
static const std::vector<uint8_t> kFakeBC1 = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
};

// ===========================================================================
// 1. Raw (None) second-stage compressor — Hap (BC1)
// ===========================================================================
TEST(HapFrameParserTest, RawHapBC1RoundTrip) {
    // Section type 0xAB = Hap, DXT1, None compressor
    std::vector<uint8_t> frame;
    appendHeader(frame, 0xAB, static_cast<uint32_t>(kFakeBC1.size()));
    frame.insert(frame.end(), kFakeBC1.begin(), kFakeBC1.end());

    HapDecodedFrame result = parseHapFrame(frame.data(), frame.size());

    EXPECT_TRUE(result.valid());
    EXPECT_EQ(result.format, HapTextureFormat::RGB_DXT1);
    ASSERT_EQ(result.textureData.size(), kFakeBC1.size());
    EXPECT_EQ(result.textureData, kFakeBC1);
    EXPECT_TRUE(result.alphaPlane.empty());
}

// ===========================================================================
// 2. Raw Hap Alpha (BC3)
// ===========================================================================
TEST(HapFrameParserTest, RawHapAlphaBC3RoundTrip) {
    // Section type 0xAE = Hap Alpha, DXT5, None compressor
    const std::vector<uint8_t> payload(16, 0xAA); // 16-byte BC3 block
    std::vector<uint8_t> frame;
    appendHeader(frame, 0xAE, static_cast<uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());

    HapDecodedFrame result = parseHapFrame(frame.data(), frame.size());

    EXPECT_TRUE(result.valid());
    EXPECT_EQ(result.format, HapTextureFormat::RGBA_DXT5);
    EXPECT_EQ(result.textureData, payload);
}

// ===========================================================================
// 3. Snappy-compressed Hap Q (YCoCg DXT5)
// ===========================================================================
TEST(HapFrameParserTest, SnappyHapQRoundTrip) {
    // Encode kFakeBC1 as a Snappy literal block, frame as section type 0xBF
    // 0xBF = Hap Q (YCoCg DXT5) + Snappy compressor
    auto snappyBlob = makeSnappyLiterals(kFakeBC1);

    std::vector<uint8_t> frame;
    appendHeader(frame, 0xBF, static_cast<uint32_t>(snappyBlob.size()));
    frame.insert(frame.end(), snappyBlob.begin(), snappyBlob.end());

    HapDecodedFrame result = parseHapFrame(frame.data(), frame.size());

    EXPECT_TRUE(result.valid());
    EXPECT_EQ(result.format, HapTextureFormat::YCoCg_DXT5);
    ASSERT_EQ(result.textureData.size(), kFakeBC1.size());
    EXPECT_EQ(result.textureData, kFakeBC1);
}

// ===========================================================================
// 4. Complex (chunked) — 2 raw chunks concatenate correctly
// ===========================================================================
TEST(HapFrameParserTest, ComplexChunked_TwoRawChunks) {
    // Build chunk data: two 8-byte raw chunks
    const std::vector<uint8_t> chunk0 = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const std::vector<uint8_t> chunk1 = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8};

    // Build the Decode Instructions Container (type 0x01).
    // Subsection 0x02: compressor table — 2 bytes (0x0A = raw)
    std::vector<uint8_t> compressors = {0x0A, 0x0A};
    // Subsection 0x03: size table — 2 × uint32_t LE
    std::vector<uint8_t> sizesBytes;
    uint32_t cs = static_cast<uint32_t>(chunk0.size());
    sizesBytes.push_back(cs & 0xFF); sizesBytes.push_back((cs >> 8) & 0xFF);
    sizesBytes.push_back((cs >> 16) & 0xFF); sizesBytes.push_back((cs >> 24) & 0xFF);
    cs = static_cast<uint32_t>(chunk1.size());
    sizesBytes.push_back(cs & 0xFF); sizesBytes.push_back((cs >> 8) & 0xFF);
    sizesBytes.push_back((cs >> 16) & 0xFF); sizesBytes.push_back((cs >> 24) & 0xFF);

    // Assemble container payload (subsections 0x02 and 0x03).
    std::vector<uint8_t> containerPayload;
    appendHeader(containerPayload, 0x02, static_cast<uint32_t>(compressors.size()));
    containerPayload.insert(containerPayload.end(), compressors.begin(), compressors.end());
    appendHeader(containerPayload, 0x03, static_cast<uint32_t>(sizesBytes.size()));
    containerPayload.insert(containerPayload.end(), sizesBytes.begin(), sizesBytes.end());

    // Top-level payload = container header (type 0x01) + container payload + chunk data
    // 0x01 = Decode Instructions Container subsection type (spec §Decode Instructions Container)
    std::vector<uint8_t> payload;
    appendHeader(payload, 0x01, static_cast<uint32_t>(containerPayload.size()));
    payload.insert(payload.end(), containerPayload.begin(), containerPayload.end());
    payload.insert(payload.end(), chunk0.begin(), chunk0.end());
    payload.insert(payload.end(), chunk1.begin(), chunk1.end());

    // Wrap in a top-level section type 0xCB = Hap (DXT1) + Decode Instructions
    std::vector<uint8_t> frame;
    appendHeader(frame, 0xCB, static_cast<uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());

    HapDecodedFrame result = parseHapFrame(frame.data(), frame.size());

    EXPECT_TRUE(result.valid());
    EXPECT_EQ(result.format, HapTextureFormat::RGB_DXT1);
    ASSERT_EQ(result.textureData.size(), chunk0.size() + chunk1.size());
    std::vector<uint8_t> expected;
    expected.insert(expected.end(), chunk0.begin(), chunk0.end());
    expected.insert(expected.end(), chunk1.begin(), chunk1.end());
    EXPECT_EQ(result.textureData, expected);
}

// ===========================================================================
// 5. Complex (chunked) — one Snappy chunk
// ===========================================================================
TEST(HapFrameParserTest, ComplexChunked_OneSnappyChunk) {
    auto snappyData = makeSnappyLiterals(kFakeBC1);

    std::vector<uint8_t> compressors = {0x0B}; // Snappy
    std::vector<uint8_t> sizesBytes;
    uint32_t cs = static_cast<uint32_t>(snappyData.size());
    sizesBytes.push_back(cs & 0xFF); sizesBytes.push_back((cs >> 8) & 0xFF);
    sizesBytes.push_back((cs >> 16) & 0xFF); sizesBytes.push_back((cs >> 24) & 0xFF);

    std::vector<uint8_t> containerPayload;
    appendHeader(containerPayload, 0x02, static_cast<uint32_t>(compressors.size()));
    containerPayload.insert(containerPayload.end(), compressors.begin(), compressors.end());
    appendHeader(containerPayload, 0x03, static_cast<uint32_t>(sizesBytes.size()));
    containerPayload.insert(containerPayload.end(), sizesBytes.begin(), sizesBytes.end());

    // 0x01 = Decode Instructions Container
    std::vector<uint8_t> payload;
    appendHeader(payload, 0x01, static_cast<uint32_t>(containerPayload.size()));
    payload.insert(payload.end(), containerPayload.begin(), containerPayload.end());
    payload.insert(payload.end(), snappyData.begin(), snappyData.end());

    std::vector<uint8_t> frame;
    appendHeader(frame, 0xCB, static_cast<uint32_t>(payload.size())); // Hap DXT1 Complex
    frame.insert(frame.end(), payload.begin(), payload.end());

    HapDecodedFrame result = parseHapFrame(frame.data(), frame.size());

    EXPECT_TRUE(result.valid());
    EXPECT_EQ(result.format, HapTextureFormat::RGB_DXT1);
    EXPECT_EQ(result.textureData, kFakeBC1);
}

// ===========================================================================
// 6. Extended (8-byte) header — section size > 0x00FFFFFF uses zero-size form
// ===========================================================================
TEST(HapFrameParserTest, ExtendedHeaderRoundTrip) {
    // Build a payload whose size forces the 8-byte extended header path.
    // We use a manually constructed 8-byte header (size24 = 0 → read uint32).
    const std::vector<uint8_t> payload(16, 0x55);
    std::vector<uint8_t> frame;
    // Extended header: 3 zeros, type 0xAB, then 4-byte LE size
    frame.push_back(0x00);
    frame.push_back(0x00);
    frame.push_back(0x00);
    frame.push_back(0xAB); // type: Hap DXT1 None
    uint32_t sz = static_cast<uint32_t>(payload.size());
    frame.push_back(sz & 0xFF);
    frame.push_back((sz >> 8) & 0xFF);
    frame.push_back((sz >> 16) & 0xFF);
    frame.push_back((sz >> 24) & 0xFF);
    frame.insert(frame.end(), payload.begin(), payload.end());

    HapDecodedFrame result = parseHapFrame(frame.data(), frame.size());
    EXPECT_TRUE(result.valid());
    EXPECT_EQ(result.format, HapTextureFormat::RGB_DXT1);
    EXPECT_EQ(result.textureData, payload);
}

// ===========================================================================
// 7. Bounds / malformed — truncated header (< 4 bytes)
// ===========================================================================
TEST(HapFrameParserTest, Malformed_TruncatedHeader) {
    // Only 3 bytes — cannot fit a 4-byte header.
    const uint8_t buf[] = {0x08, 0x00, 0x00};
    HapDecodedFrame result = parseHapFrame(buf, sizeof(buf));
    EXPECT_FALSE(result.valid());
}

// ===========================================================================
// 8. Bounds / malformed — section size exceeds buffer
// ===========================================================================
TEST(HapFrameParserTest, Malformed_SectionSizeExceedsBuffer) {
    // Header claims 1000 bytes payload; actual buffer is only 8 bytes total.
    std::vector<uint8_t> frame;
    appendHeader(frame, 0xAB, 1000); // 4-byte header (size 1000, fits in 24-bit)
    frame.push_back(0x11);           // only 1 byte of payload provided

    HapDecodedFrame result = parseHapFrame(frame.data(), frame.size());
    EXPECT_FALSE(result.valid());
}

// ===========================================================================
// 9. Bounds / malformed — absurd chunk count in complex section
// ===========================================================================
TEST(HapFrameParserTest, Malformed_AbsurdChunkCount) {
    // A Complex section whose chunk count is implausibly large (> MAX_CHUNK_COUNT).
    // Build a minimal Decode Instructions Container with compressor table size
    // exceeding MAX_CHUNK_COUNT (65536).

    // Compressor subsection: size > 65536 will trigger the MAX_CHUNK_COUNT check.
    const uint32_t absurdCount = 100000u; // > MAX_CHUNK_COUNT
    // We can't actually build 100000 bytes of data easily; instead craft a header
    // that CLAIMS this size but provide no payload data → the Reader will reject it.
    std::vector<uint8_t> containerPayload;
    appendHeader(containerPayload, 0x02, absurdCount); // size claim, no data

    // 0x01 = Decode Instructions Container
    std::vector<uint8_t> payload;
    appendHeader(payload, 0x01, static_cast<uint32_t>(containerPayload.size()));
    payload.insert(payload.end(), containerPayload.begin(), containerPayload.end());

    std::vector<uint8_t> frame;
    appendHeader(frame, 0xCB, static_cast<uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());

    HapDecodedFrame result = parseHapFrame(frame.data(), frame.size());
    EXPECT_FALSE(result.valid());
}

// ===========================================================================
// 10. Bounds / malformed — null pointer
// ===========================================================================
TEST(HapFrameParserTest, Malformed_NullPointer) {
    HapDecodedFrame result = parseHapFrame(nullptr, 16);
    EXPECT_FALSE(result.valid());
}

// ===========================================================================
// 11. Bounds / malformed — zero size
// ===========================================================================
TEST(HapFrameParserTest, Malformed_ZeroSize) {
    const uint8_t buf[] = {0xAB, 0xCD};
    HapDecodedFrame result = parseHapFrame(buf, 0);
    EXPECT_FALSE(result.valid());
}

// ===========================================================================
// 12. Bounds / malformed — unknown section type
// ===========================================================================
TEST(HapFrameParserTest, Malformed_UnknownSectionType) {
    // Low nibble 0x09 is not a known texture format.
    std::vector<uint8_t> frame;
    appendHeader(frame, 0xA9, 4);
    frame.insert(frame.end(), {0x01, 0x02, 0x03, 0x04});

    HapDecodedFrame result = parseHapFrame(frame.data(), frame.size());
    EXPECT_FALSE(result.valid());
}

// ===========================================================================
// 13. Snappy unit — SnappyDecoder round-trip
// ===========================================================================
TEST(SnappyDecoderTest, LiteralsRoundTrip) {
    const std::vector<uint8_t> original = {
        0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x57, 0x6F, 0x72, 0x6C, 0x64 // "Hello World"
    };
    auto snappyData = makeSnappyLiterals(original);

    std::vector<uint8_t> out;
    EXPECT_TRUE(decompressSnappy(snappyData.data(), snappyData.size(), out));
    EXPECT_EQ(out, original);
}

// ===========================================================================
// 14. Snappy — truncated input rejected
// ===========================================================================
TEST(SnappyDecoderTest, TruncatedInputRejected) {
    const std::vector<uint8_t> original = {0x01, 0x02, 0x03, 0x04};
    auto snappyData = makeSnappyLiterals(original);

    // Provide only half the bytes.
    std::vector<uint8_t> out;
    bool ok = decompressSnappy(snappyData.data(), snappyData.size() / 2, out);
    EXPECT_FALSE(ok);
}

// ===========================================================================
// 15. Snappy — null input rejected
// ===========================================================================
TEST(SnappyDecoderTest, NullInputRejected) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(decompressSnappy(nullptr, 10, out));
}

// ===========================================================================
// 16. Snappy — empty payload (0-byte uncompressed data)
// ===========================================================================
TEST(SnappyDecoderTest, EmptyPayload) {
    // Varint encoding of 0.
    const uint8_t buf[] = {0x00};
    std::vector<uint8_t> out;
    bool ok = decompressSnappy(buf, sizeof(buf), out);
    // Should decode successfully to empty output.
    EXPECT_TRUE(ok);
    EXPECT_TRUE(out.empty());
}

// A header declaring 1,000,000 bytes for a 5-byte input is refused before any output is
// allocated.
TEST(SnappyDecoderTest, ImplausibleDeclaredLengthRejectedBeforeAllocating) {
    const uint8_t buf[] = {0xC0, 0x84, 0x3D, 0x00, 'x'};  // varint 1000000, one literal byte
    std::vector<uint8_t> out;
    EXPECT_FALSE(decompressSnappy(buf, sizeof(buf), out));
    EXPECT_TRUE(out.empty());
}

// A chunk-offset table may point every chunk at the same bytes: the decoded size must be
// capped, not grow without bound (a hostile file would otherwise exhaust memory).
TEST(HapFrameParserTest, Malformed_OverlappingChunksExceedingTheDecodedLimitAreRejected) {
    const size_t chunkBytes = 4u * 1024u * 1024u;
    const uint32_t chunkCount = 40;  // 160 MiB of output from a 4 MiB payload
    auto le32 = [](std::vector<uint8_t>& v, uint32_t x) {
        for (int s = 0; s < 32; s += 8) v.push_back(static_cast<uint8_t>(x >> s));
    };
    std::vector<uint8_t> compressors(chunkCount, 0x0A), sizes, offsets;
    for (uint32_t i = 0; i < chunkCount; ++i) {
        le32(sizes, static_cast<uint32_t>(chunkBytes));
        le32(offsets, 0);  // every chunk is the same bytes
    }
    std::vector<uint8_t> container;
    appendHeader(container, 0x02, static_cast<uint32_t>(compressors.size()));
    container.insert(container.end(), compressors.begin(), compressors.end());
    appendHeader(container, 0x03, static_cast<uint32_t>(sizes.size()));
    container.insert(container.end(), sizes.begin(), sizes.end());
    appendHeader(container, 0x04, static_cast<uint32_t>(offsets.size()));
    container.insert(container.end(), offsets.begin(), offsets.end());

    std::vector<uint8_t> payload;
    appendHeader(payload, 0x01, static_cast<uint32_t>(container.size()));
    payload.insert(payload.end(), container.begin(), container.end());
    payload.insert(payload.end(), chunkBytes, 0x5A);

    std::vector<uint8_t> frame;
    appendHeader(frame, 0xCB, static_cast<uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());

    EXPECT_FALSE(parseHapFrame(frame.data(), frame.size()).valid());
}
