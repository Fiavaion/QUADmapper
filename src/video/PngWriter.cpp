#include "PngWriter.hpp"

#include <fstream>
#include <vector>
#include <algorithm>
#include <cstring>

// Self-contained PNG writer (no FFmpeg / zlib dependency). The default vcpkg
// FFmpeg build ships no PNG encoder, so we emit a valid PNG directly using
// uncompressed ("stored") DEFLATE blocks. Larger files, but a single pre-show
// capture is trivial and every PNG reader (incl. OpenCV/Pillow) accepts it.
namespace quadmapper {
namespace {

uint32_t crc32Bytes(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t adler32Bytes(const uint8_t* data, size_t len) {
    const uint32_t MOD = 65521;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % MOD;
        b = (b + a) % MOD;
    }
    return (b << 16) | a;
}

void putBE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void writeChunk(std::ofstream& out, const char* type, const std::vector<uint8_t>& data) {
    uint8_t len[4] = {
        static_cast<uint8_t>(data.size() >> 24), static_cast<uint8_t>(data.size() >> 16),
        static_cast<uint8_t>(data.size() >> 8),  static_cast<uint8_t>(data.size())
    };
    out.write(reinterpret_cast<const char*>(len), 4);
    out.write(type, 4);
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    std::vector<uint8_t> crcbuf;
    crcbuf.insert(crcbuf.end(), type, type + 4);
    crcbuf.insert(crcbuf.end(), data.begin(), data.end());
    uint32_t crc = crc32Bytes(crcbuf.data(), crcbuf.size());
    uint8_t crcb[4] = {
        static_cast<uint8_t>(crc >> 24), static_cast<uint8_t>(crc >> 16),
        static_cast<uint8_t>(crc >> 8),  static_cast<uint8_t>(crc)
    };
    out.write(reinterpret_cast<const char*>(crcb), 4);
}

} // namespace

bool writeRgbPng(const uint8_t* rgb, int width, int height, const std::filesystem::path& path) {
    if (!rgb || width <= 0 || height <= 0) {
        return false;
    }

    // Filtered raw scanlines: each row prefixed with filter-type byte 0 (None).
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 3));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgb + static_cast<size_t>(y) * width * 3;
        raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 3);
    }

    // zlib stream: 2-byte header + stored DEFLATE blocks + 4-byte Adler-32.
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    const size_t MAX_BLOCK = 65535;
    size_t pos = 0;
    do {
        size_t n = std::min(MAX_BLOCK, raw.size() - pos);
        bool final = (pos + n >= raw.size());
        z.push_back(final ? 1 : 0);                 // BFINAL + BTYPE=00 (stored)
        uint16_t len = static_cast<uint16_t>(n);
        uint16_t nlen = static_cast<uint16_t>(~len);
        z.push_back(len & 0xFF);
        z.push_back((len >> 8) & 0xFF);
        z.push_back(nlen & 0xFF);
        z.push_back((nlen >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    putBE32(z, adler32Bytes(raw.data(), raw.size()));

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    out.write(reinterpret_cast<const char*>(sig), 8);

    std::vector<uint8_t> ihdr;
    putBE32(ihdr, static_cast<uint32_t>(width));
    putBE32(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(2);   // color type 2 = truecolor RGB
    ihdr.push_back(0);   // compression
    ihdr.push_back(0);   // filter
    ihdr.push_back(0);   // interlace
    writeChunk(out, "IHDR", ihdr);
    writeChunk(out, "IDAT", z);
    writeChunk(out, "IEND", {});

    return out.good();
}

} // namespace quadmapper
