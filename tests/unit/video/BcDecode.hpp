#pragma once

// Test-only reference decoding of what a HAP frame puts on the GPU: BC1 and BC3
// blocks decoded as the S3TC spec reads them, and Hap Q's scaled YCoCg turned back
// into RGB with the arithmetic of quad.frag. Plus PSNR, to compare with the source.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace quadmapper::bctest {

inline void expand565(uint16_t c, int rgb[3]) {
    const int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

// One 8-byte colour block into 16 RGBA texels. alwaysFourColours: BC3's colour half.
inline void decodeColourBlock(const uint8_t* block, bool alwaysFourColours, uint8_t out[16][4]) {
    const uint16_t c0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
    const uint16_t c1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
    int palette[4][4];
    expand565(c0, palette[0]);
    expand565(c1, palette[1]);
    palette[0][3] = palette[1][3] = 255;
    const bool four = alwaysFourColours || c0 > c1;
    for (int ch = 0; ch < 3; ++ch) {
        if (four) {
            palette[2][ch] = (2 * palette[0][ch] + palette[1][ch]) / 3;
            palette[3][ch] = (palette[0][ch] + 2 * palette[1][ch]) / 3;
        } else {
            palette[2][ch] = (palette[0][ch] + palette[1][ch]) / 2;
            palette[3][ch] = 0;
        }
    }
    palette[2][3] = 255;
    palette[3][3] = four ? 255 : 0;
    const uint32_t indices = static_cast<uint32_t>(block[4]) | (block[5] << 8) | (block[6] << 16) |
                             (static_cast<uint32_t>(block[7]) << 24);
    for (int i = 0; i < 16; ++i) {
        const int index = (indices >> (2 * i)) & 3;
        for (int ch = 0; ch < 4; ++ch) out[i][ch] = static_cast<uint8_t>(palette[index][ch]);
    }
}

inline void decodeAlphaBlock(const uint8_t* block, uint8_t out[16][4]) {
    const int a0 = block[0], a1 = block[1];
    int palette[8] = {a0, a1};
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i) palette[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int i = 1; i < 5; ++i) palette[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        palette[6] = 0;
        palette[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i][3] = static_cast<uint8_t>(palette[(bits >> (3 * i)) & 7]);
}

// BC1 (bc3 false) or BC3 texture data of a width x height image into RGBA.
inline std::vector<uint8_t> decodeBlocks(const std::vector<uint8_t>& data, int width, int height, bool bc3) {
    const int blocksWide = (width + 3) / 4, blocksHigh = (height + 3) / 4;
    const size_t blockBytes = bc3 ? 16 : 8;
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    if (data.size() < blockBytes * blocksWide * blocksHigh) return {};
    uint8_t texels[16][4];
    for (int by = 0; by < blocksHigh; ++by) {
        for (int bx = 0; bx < blocksWide; ++bx) {
            const uint8_t* block = data.data() + (static_cast<size_t>(by) * blocksWide + bx) * blockBytes;
            if (bc3) {
                decodeColourBlock(block + 8, true, texels);
                decodeAlphaBlock(block, texels);
            } else {
                decodeColourBlock(block, false, texels);
            }
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x < width && y < height) std::memcpy(&rgba[(static_cast<size_t>(y) * width + x) * 4], texels[i], 4);
            }
        }
    }
    return rgba;
}

// Hap Q: quad.frag's scaled-YCoCg to RGB, on decoded BC3 texels.
inline void ycocgToRgb(std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const double scale = rgba[i + 2] / 255.0 * (255.0 / 8.0) + 1.0;
        const double co = (rgba[i + 0] / 255.0 - 0.5 * 256.0 / 255.0) / scale;
        const double cg = (rgba[i + 1] / 255.0 - 0.5 * 256.0 / 255.0) / scale;
        const double y = rgba[i + 3] / 255.0;
        const double rgb[3] = {y + co - cg, y + cg, y - co - cg};
        for (int ch = 0; ch < 3; ++ch) {
            rgba[i + ch] = static_cast<uint8_t>(std::lround(std::clamp(rgb[ch], 0.0, 1.0) * 255.0));
        }
        rgba[i + 3] = 255;
    }
}

// PSNR in dB over the RGB channels; a and b hold aBytes / bBytes per pixel.
inline double psnr(const uint8_t* a, int aBytes, const uint8_t* b, int bBytes, size_t pixels) {
    double sum = 0.0;
    for (size_t p = 0; p < pixels; ++p) {
        for (int ch = 0; ch < 3; ++ch) {
            const double d = double(a[p * aBytes + ch]) - double(b[p * bBytes + ch]);
            sum += d * d;
        }
    }
    const double mse = sum / (3.0 * static_cast<double>(pixels));
    return mse <= 0.0 ? std::numeric_limits<double>::infinity() : 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace quadmapper::bctest
