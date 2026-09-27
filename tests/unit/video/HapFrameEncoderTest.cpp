// HAP frame encoder (QM-082): frames it writes parse with our HAP parser (the one
// playback uses), decode back close to the source image, and Snappy data it writes
// decompresses exactly.

#include <gtest/gtest.h>

#include "BcDecode.hpp"
#include "source/hap/HapFrameParser.hpp"
#include "source/hap/snappy/SnappyDecoder.hpp"
#include "video/HapFrameEncoder.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace quadmapper;
using namespace quadmapper::hap;

namespace {

// Smooth gradients with a hard-edged disc and a few thin lines: the mix of flat,
// graded and sharp detail real footage has.
std::vector<uint8_t> testImage(int width, int height) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* p = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            const double dx = x - width * 0.6, dy = y - height * 0.4;
            const bool disc = dx * dx + dy * dy < (height * 0.25) * (height * 0.25);
            p[0] = static_cast<uint8_t>(disc ? 230 : 255 * x / width);
            p[1] = static_cast<uint8_t>(disc ? 40 : 255 * y / height);
            p[2] = static_cast<uint8_t>(disc ? 60 : 128 + 60 * std::sin(x * 0.05));
            if (x % 29 == 0) p[0] = p[1] = p[2] = 250;
            p[3] = 255;
        }
    }
    return rgba;
}

std::vector<uint8_t> decodeFrame(const std::vector<uint8_t>& frame, int width, int height,
                                 HapTextureFormat expected) {
    const HapDecodedFrame decoded = parseHapFrame(frame.data(), frame.size());
    EXPECT_TRUE(decoded.valid());
    EXPECT_EQ(decoded.format, expected);
    const bool q = expected == HapTextureFormat::YCoCg_DXT5;
    EXPECT_EQ(decoded.textureData.size(), hapTextureSize(width, height, q ? HapVariant::HapQ : HapVariant::Hap));
    std::vector<uint8_t> rgba = bctest::decodeBlocks(decoded.textureData, width, height, q);
    if (q) bctest::ycocgToRgb(rgba);
    return rgba;
}

std::vector<uint8_t> roundTripSnappy(const std::vector<uint8_t>& data) {
    const std::vector<uint8_t> compressed = snappyCompress(data.data(), data.size());
    std::vector<uint8_t> out;
    EXPECT_TRUE(snappy::decompressSnappy(compressed.data(), compressed.size(), out));
    return out;
}

}  // namespace

TEST(HapSnappy, RoundTripsEveryShape) {
    EXPECT_EQ(roundTripSnappy({}), std::vector<uint8_t>{});
    EXPECT_EQ(roundTripSnappy({7}), std::vector<uint8_t>{7});
    const std::vector<uint8_t> tiny{1, 2, 3, 1, 2, 3, 1};
    EXPECT_EQ(roundTripSnappy(tiny), tiny);

    // Several 64 KiB blocks of noise (literals of every length), runs (overlapping
    // copies longer than 64 bytes) and a repeated pattern (short- and long-offset copies).
    std::vector<uint8_t> mixed;
    uint32_t seed = 12345;
    for (int i = 0; i < 90000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        mixed.push_back(static_cast<uint8_t>(seed >> 24));
    }
    mixed.insert(mixed.end(), 70000, 0xAB);
    for (int i = 0; i < 50000; ++i) mixed.push_back(static_cast<uint8_t>(i % 3000 * 7));
    EXPECT_EQ(roundTripSnappy(mixed), mixed);
}

TEST(HapSnappy, CompressesRepetitiveData) {
    const std::vector<uint8_t> runs(200000, 0x5A);
    EXPECT_LT(snappyCompress(runs.data(), runs.size()).size(), runs.size() / 10);
}

// PSNR floors: BC1 keeps 16-bit endpoint colours and four levels per 4x4 block,
// which on graded content with hard edges lands in the mid 30s dB; 30 dB leaves room
// for harder footage while still failing any wrong channel order or block layout
// (those land below 15 dB). Hap Q's BC3 keeps luma at 8 bits with eight levels per
// block and scales low-chroma blocks up, so it is several dB better: floor 35 dB.
constexpr double HAP_MIN_PSNR = 30.0;
constexpr double HAPQ_MIN_PSNR = 35.0;

TEST(HapFrameEncoder, HapFrameDecodesCloseToTheSource) {
    constexpr int W = 128, H = 72;
    const std::vector<uint8_t> image = testImage(W, H);
    const std::vector<uint8_t> frame = encodeHapFrame(image.data(), W, H, W * 4, HapVariant::Hap, 1);
    const std::vector<uint8_t> decoded = decodeFrame(frame, W, H, HapTextureFormat::RGB_DXT1);
    const double db = bctest::psnr(decoded.data(), 4, image.data(), 4, W * H);
    RecordProperty("psnr_db", std::to_string(db));
    EXPECT_GE(db, HAP_MIN_PSNR);
}

TEST(HapFrameEncoder, HapQFrameDecodesCloseToTheSourceThroughTheShaderMaths) {
    constexpr int W = 128, H = 72;
    const std::vector<uint8_t> image = testImage(W, H);
    const std::vector<uint8_t> frame = encodeHapFrame(image.data(), W, H, W * 4, HapVariant::HapQ, 1);
    const std::vector<uint8_t> decoded = decodeFrame(frame, W, H, HapTextureFormat::YCoCg_DXT5);
    const double db = bctest::psnr(decoded.data(), 4, image.data(), 4, W * H);
    RecordProperty("psnr_db", std::to_string(db));
    EXPECT_GE(db, HAPQ_MIN_PSNR);
}

// Sizes that are not whole blocks are padded by repeating the edge; the padding
// must not bleed into the picture.
TEST(HapFrameEncoder, SizesThatAreNotWholeBlocks) {
    for (const auto variant : {HapVariant::Hap, HapVariant::HapQ}) {
        constexpr int W = 70, H = 42;
        const std::vector<uint8_t> image = testImage(W, H);
        const std::vector<uint8_t> frame = encodeHapFrame(image.data(), W, H, W * 4, variant, 3);
        const auto format = variant == HapVariant::Hap ? HapTextureFormat::RGB_DXT1 : HapTextureFormat::YCoCg_DXT5;
        const std::vector<uint8_t> decoded = decodeFrame(frame, W, H, format);
        EXPECT_GE(bctest::psnr(decoded.data(), 4, image.data(), 4, W * H),
                  variant == HapVariant::Hap ? HAP_MIN_PSNR : HAPQ_MIN_PSNR);
    }
}

// Threads split the image by block rows; the result is the same bytes.
TEST(HapFrameEncoder, ThreadCountDoesNotChangeTheFrame) {
    constexpr int W = 96, H = 64;
    const std::vector<uint8_t> image = testImage(W, H);
    for (const auto variant : {HapVariant::Hap, HapVariant::HapQ}) {
        EXPECT_EQ(encodeHapFrame(image.data(), W, H, W * 4, variant, 1),
                  encodeHapFrame(image.data(), W, H, W * 4, variant, 7));
    }
}

// Flat colours are nearly exact in both variants, including grey (no chroma, so Hap
// Q's largest chroma scale) and a saturated colour (scale 1).
TEST(HapFrameEncoder, FlatColoursAreNearlyExact) {
    constexpr int W = 16, H = 8;
    for (const auto variant : {HapVariant::Hap, HapVariant::HapQ}) {
        for (const auto& colour : std::vector<std::vector<uint8_t>>{{128, 128, 128}, {250, 20, 30}, {0, 0, 0}}) {
            std::vector<uint8_t> image;
            for (int i = 0; i < W * H; ++i) image.insert(image.end(), {colour[0], colour[1], colour[2], 255});
            const std::vector<uint8_t> frame = encodeHapFrame(image.data(), W, H, W * 4, variant, 1);
            const auto format = variant == HapVariant::Hap ? HapTextureFormat::RGB_DXT1 : HapTextureFormat::YCoCg_DXT5;
            const std::vector<uint8_t> decoded = decodeFrame(frame, W, H, format);
            ASSERT_EQ(decoded.size(), image.size());
            for (int ch = 0; ch < 3; ++ch) {
                EXPECT_NEAR(decoded[ch], colour[ch], 5) << "variant " << int(variant) << " channel " << ch;
            }
        }
    }
}

// The frame is stored raw when Snappy would not make it smaller, and parses either way.
TEST(HapFrameEncoder, SectionTypeNamesTheCompressor) {
    constexpr int W = 64, H = 64;
    const std::vector<uint8_t> flat(W * H * 4, 90);
    const std::vector<uint8_t> frame = encodeHapFrame(flat.data(), W, H, W * 4, HapVariant::Hap, 1);
    ASSERT_GE(frame.size(), 4u);
    EXPECT_EQ(frame[3], 0xBB) << "flat texture data compresses: Snappy, DXT1";
    EXPECT_LT(frame.size(), hapTextureSize(W, H, HapVariant::Hap));

    std::vector<uint8_t> noise(W * H * 4);
    uint32_t seed = 99;
    for (auto& b : noise) {
        seed = seed * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(seed >> 24);
    }
    const std::vector<uint8_t> noisy = encodeHapFrame(noise.data(), W, H, W * 4, HapVariant::HapQ, 1);
    EXPECT_TRUE(noisy[3] == 0xAF || noisy[3] == 0xBF);
    EXPECT_TRUE(parseHapFrame(noisy.data(), noisy.size()).valid());
}
