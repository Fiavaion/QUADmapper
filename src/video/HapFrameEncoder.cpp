#include "HapFrameEncoder.hpp"

#include <stb_dxt.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace quadmapper {
namespace hap {

namespace {

// Section-type byte: second-stage compressor (high nibble) | texture format (low).
constexpr uint8_t COMPRESSOR_NONE = 0xA0;
constexpr uint8_t COMPRESSOR_SNAPPY = 0xB0;
constexpr uint8_t FORMAT_RGB_DXT1 = 0x0B;
constexpr uint8_t FORMAT_YCOCG_DXT5 = 0x0F;

size_t blockBytes(HapVariant variant) { return variant == HapVariant::Hap ? 8 : 16; }

// The 4x4 block at (bx, by), edge pixels repeated past the image, as RGBA.
// (Plain loops without std::min/abs helpers: this runs per pixel, and Debug builds
// do not inline them.)
void gatherBlock(const uint8_t* rgba, int width, int height, size_t stride, int bx, int by,
                 uint8_t block[64]) {
    for (int y = 0; y < 4; ++y) {
        const int sy = by * 4 + y < height ? by * 4 + y : height - 1;
        const uint8_t* row = rgba + static_cast<size_t>(sy) * stride;
        for (int x = 0; x < 4; ++x) {
            const int sx = bx * 4 + x < width ? bx * 4 + x : width - 1;
            const uint8_t* p = row + static_cast<size_t>(sx) * 4;
            uint8_t* out = block + (y * 4 + x) * 4;
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            out[3] = 255;
        }
    }
}

// Scaled YCoCg (J.M.P. van Waveren and I. Castano, "Real-Time YCoCg-DXT Compression",
// 2007), the layout Hap Q stores: r = Co, g = Cg, b = (scale - 1) * 8, a = Y. The
// chroma of a block with little colour is multiplied by 2 or 4 before compression to
// use more of the 8 bits. quad.frag reverses it. Every value stays within 0..255:
// Co and Cg lie in 128 +- 128, and a block is scaled only while that stays true.
void toScaledYCoCg(uint8_t block[64], int& scale) {
    int extent = 0;
    for (int i = 0; i < 16; ++i) {
        uint8_t* p = block + i * 4;
        const int r = p[0], g = p[1], b = p[2];
        const int co = (r - b) >> 1;
        const int cg = (-r + 2 * g - b) >> 2;
        p[0] = static_cast<uint8_t>(co + 128);
        p[1] = static_cast<uint8_t>(cg + 128);
        p[3] = static_cast<uint8_t>((r + 2 * g + b) >> 2);  // Y
        const int ac = co < 0 ? -co : co;
        const int ag = cg < 0 ? -cg : cg;
        if (ac > extent) extent = ac;
        if (ag > extent) extent = ag;
    }
    scale = extent <= 31 ? 4 : extent <= 63 ? 2 : 1;
    const uint8_t blue = static_cast<uint8_t>((scale - 1) << 3);
    for (int i = 0; i < 16; ++i) {
        uint8_t* p = block + i * 4;
        p[0] = static_cast<uint8_t>((p[0] - 128) * scale + 128);
        p[1] = static_cast<uint8_t>((p[1] - 128) * scale + 128);
        p[2] = blue;
    }
}

// Write the scale exactly into both colour endpoints' 5-bit blue (0, 1 or 3): the
// compressor may approximate a constant channel with two nearby endpoints, and the
// decoder reads the scale back from blue. Keeps color0 > color1 so no decoder can
// read the block in DXT1's three-colour mode.
void setBlueScale(uint8_t colorBlock[8], int scale) {
    auto read16 = [&](int at) {
        return static_cast<uint16_t>(colorBlock[at] | (colorBlock[at + 1] << 8));
    };
    auto write16 = [&](int at, uint16_t v) {
        colorBlock[at] = static_cast<uint8_t>(v & 0xFF);
        colorBlock[at + 1] = static_cast<uint8_t>(v >> 8);
    };
    const uint16_t blue = static_cast<uint16_t>(scale - 1);
    uint16_t c0 = static_cast<uint16_t>((read16(0) & ~0x1Fu) | blue);
    uint16_t c1 = static_cast<uint16_t>((read16(2) & ~0x1Fu) | blue);
    uint32_t mask = static_cast<uint32_t>(colorBlock[4]) | (colorBlock[5] << 8) |
                    (colorBlock[6] << 16) | (static_cast<uint32_t>(colorBlock[7]) << 24);
    if (c0 < c1) {
        std::swap(c0, c1);
        mask ^= 0x55555555u;  // index 0 <-> 1 and 2 <-> 3 describe the same colours
    } else if (c0 == c1) {
        mask = 0;  // one colour: every texel index 0
    }
    write16(0, c0);
    write16(2, c1);
    for (int i = 0; i < 4; ++i) colorBlock[4 + i] = static_cast<uint8_t>(mask >> (8 * i));
}

void compressRows(const uint8_t* rgba, int width, int height, size_t stride, HapVariant variant,
                  int blocksWide, int rowBegin, int rowEnd, uint8_t* out) {
    const size_t bytes = blockBytes(variant);
    uint8_t block[64];
    for (int by = rowBegin; by < rowEnd; ++by) {
        uint8_t* dest = out + static_cast<size_t>(by) * blocksWide * bytes;
        for (int bx = 0; bx < blocksWide; ++bx, dest += bytes) {
            gatherBlock(rgba, width, height, stride, bx, by, block);
            if (variant == HapVariant::Hap) {
                stb_compress_dxt_block(dest, block, 0, STB_DXT_HIGHQUAL);
            } else {
                int scale = 1;
                toScaledYCoCg(block, scale);
                stb_compress_dxt_block(dest, block, 1, STB_DXT_HIGHQUAL);
                setBlueScale(dest + 8, scale);
            }
        }
    }
}

void appendSectionHeader(std::vector<uint8_t>& out, size_t payloadSize, uint8_t type) {
    if (payloadSize <= 0xFFFFFF) {
        out.push_back(static_cast<uint8_t>(payloadSize & 0xFF));
        out.push_back(static_cast<uint8_t>((payloadSize >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((payloadSize >> 16) & 0xFF));
        out.push_back(type);
        return;
    }
    // Too big for 24 bits: zero size, then the size as 32 bits.
    out.insert(out.end(), {0, 0, 0, type});
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((payloadSize >> (8 * i)) & 0xFF));
}

// --- Snappy ---

constexpr size_t SNAPPY_BLOCK = size_t(1) << 16;  // back-references stay within one
constexpr int SNAPPY_HASH_BITS = 14;

uint32_t load32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

void emitLiteral(std::vector<uint8_t>& out, const uint8_t* data, size_t length) {
    if (length == 0) return;
    const size_t n = length - 1;  // at most 65535: literals never cross a block
    if (n < 60) {
        out.push_back(static_cast<uint8_t>(n << 2));
    } else if (n < 256) {
        out.push_back(60 << 2);
        out.push_back(static_cast<uint8_t>(n));
    } else {
        out.push_back(61 << 2);
        out.push_back(static_cast<uint8_t>(n & 0xFF));
        out.push_back(static_cast<uint8_t>(n >> 8));
    }
    out.insert(out.end(), data, data + length);
}

// One copy element of 4..64 bytes; offset < 65536.
void emitShortCopy(std::vector<uint8_t>& out, size_t offset, size_t length) {
    if (length < 12 && offset < 2048) {
        out.push_back(static_cast<uint8_t>(1 | ((length - 4) << 2) | ((offset >> 8) << 5)));
        out.push_back(static_cast<uint8_t>(offset & 0xFF));
    } else {
        out.push_back(static_cast<uint8_t>(2 | ((length - 1) << 2)));
        out.push_back(static_cast<uint8_t>(offset & 0xFF));
        out.push_back(static_cast<uint8_t>(offset >> 8));
    }
}

void emitCopy(std::vector<uint8_t>& out, size_t offset, size_t length) {
    while (length >= 68) {
        emitShortCopy(out, offset, 64);
        length -= 64;
    }
    if (length > 64) {  // leave at least 4 for the last element
        emitShortCopy(out, offset, 60);
        length -= 60;
    }
    emitShortCopy(out, offset, length);
}

} // namespace

size_t hapTextureSize(int width, int height, HapVariant variant) {
    if (width <= 0 || height <= 0) return 0;
    return static_cast<size_t>((width + 3) / 4) * static_cast<size_t>((height + 3) / 4) * blockBytes(variant);
}

std::vector<uint8_t> encodeHapFrame(const uint8_t* rgba, int width, int height, size_t stride,
                                    HapVariant variant, int threads) {
    const size_t textureSize = hapTextureSize(width, height, variant);
    if (!rgba || textureSize == 0) return {};

    const int blocksWide = (width + 3) / 4;
    const int blocksHigh = (height + 3) / 4;
    std::vector<uint8_t> texture(textureSize);
    const int workers = std::clamp(threads, 1, blocksHigh);
    if (workers == 1) {
        compressRows(rgba, width, height, stride, variant, blocksWide, 0, blocksHigh, texture.data());
    } else {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(workers));
        for (int w = 0; w < workers; ++w) {
            const int begin = blocksHigh * w / workers;
            const int end = blocksHigh * (w + 1) / workers;
            pool.emplace_back(compressRows, rgba, width, height, stride, variant, blocksWide, begin, end,
                              texture.data());
        }
        for (auto& t : pool) t.join();
    }

    const uint8_t format = variant == HapVariant::Hap ? FORMAT_RGB_DXT1 : FORMAT_YCOCG_DXT5;
    std::vector<uint8_t> compressed = snappyCompress(texture.data(), texture.size());
    const bool useSnappy = compressed.size() < texture.size();
    const std::vector<uint8_t>& payload = useSnappy ? compressed : texture;

    std::vector<uint8_t> frame;
    frame.reserve(payload.size() + 8);
    appendSectionHeader(frame, payload.size(),
                        static_cast<uint8_t>((useSnappy ? COMPRESSOR_SNAPPY : COMPRESSOR_NONE) | format));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<uint8_t> snappyCompress(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size + size / 6 + 32);
    for (size_t v = size;; v >>= 7) {  // uncompressed length, little-endian base-128
        if (v < 0x80) {
            out.push_back(static_cast<uint8_t>(v));
            break;
        }
        out.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
    }

    std::vector<uint16_t> table(size_t(1) << SNAPPY_HASH_BITS);
    for (size_t blockStart = 0; blockStart < size; blockStart += SNAPPY_BLOCK) {
        const size_t blockEnd = std::min(size, blockStart + SNAPPY_BLOCK);
        std::fill(table.begin(), table.end(), uint16_t(0));
        size_t literalStart = blockStart;
        size_t ip = blockStart + 1;
        size_t misses = 0;
        // A match needs four readable bytes at ip.
        while (blockEnd - blockStart >= 8 && ip + 4 <= blockEnd) {
            const uint32_t bytes = load32(data + ip);
            const uint32_t hash = (bytes * 0x1E35A7BDu) >> (32 - SNAPPY_HASH_BITS);
            const size_t candidate = blockStart + table[hash];
            table[hash] = static_cast<uint16_t>(ip - blockStart);
            if (candidate >= ip || load32(data + candidate) != bytes) {
                ip += 1 + (misses++ >> 5);  // skip faster through data that does not repeat
                continue;
            }
            size_t length = 4;
            while (ip + length < blockEnd && data[candidate + length] == data[ip + length]) ++length;
            emitLiteral(out, data + literalStart, ip - literalStart);
            emitCopy(out, ip - candidate, length);
            ip += length;
            literalStart = ip;
            misses = 0;
        }
        emitLiteral(out, data + literalStart, blockEnd - literalStart);
    }
    return out;
}

} // namespace hap
} // namespace quadmapper
