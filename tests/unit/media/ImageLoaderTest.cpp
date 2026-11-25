#include <gtest/gtest.h>
#include "media/ImageLoader.hpp"
#include "media/MediaLibrary.hpp"
#include "core/FileSystem.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace quadmapper {
namespace test {

namespace {

// One page of a baseline (uncompressed, chunky, single-strip) little-endian TIFF.
// Rows are given top row first, as TIFF stores them by default.
struct TiffPage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t samplesPerPixel = 3;
    uint16_t bitsPerSample = 8;
    uint16_t photometric = 2;   // 1 = BlackIsZero gray, 2 = RGB
    bool unassociatedAlpha = false;
    std::vector<uint8_t> data;  // 16-bit samples little-endian
};

void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void set32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

void padEven(std::vector<uint8_t>& out) {
    if (out.size() % 2) out.push_back(0);
}

std::vector<uint8_t> buildTiff(const std::vector<TiffPage>& pages) {
    std::vector<uint8_t> out = {'I', 'I', 42, 0};
    size_t nextIfdLink = out.size();
    put32(out, 0);

    for (const TiffPage& p : pages) {
        const auto stripOffset = static_cast<uint32_t>(out.size());
        out.insert(out.end(), p.data.begin(), p.data.end());
        padEven(out);

        // BitsPerSample holds one SHORT per sample: inline when it fits in 4 bytes.
        uint32_t bitsValue = p.bitsPerSample;
        if (p.samplesPerPixel == 2) bitsValue = p.bitsPerSample | (p.bitsPerSample << 16);
        if (p.samplesPerPixel > 2) {
            bitsValue = static_cast<uint32_t>(out.size());
            for (int i = 0; i < p.samplesPerPixel; ++i) put16(out, p.bitsPerSample);
        }

        struct Entry { uint16_t tag, type; uint32_t count, value; };
        std::vector<Entry> entries = {
            {256, 4, 1, p.width},
            {257, 4, 1, p.height},
            {258, 3, p.samplesPerPixel, bitsValue},
            {259, 3, 1, 1},
            {262, 3, 1, p.photometric},
            {273, 4, 1, stripOffset},
            {277, 3, 1, p.samplesPerPixel},
            {278, 4, 1, p.height},
            {279, 4, 1, static_cast<uint32_t>(p.data.size())},
            {284, 3, 1, 1},
        };
        if (p.unassociatedAlpha) entries.push_back({338, 3, 1, 2});

        set32(out, nextIfdLink, static_cast<uint32_t>(out.size()));
        put16(out, static_cast<uint16_t>(entries.size()));
        for (const Entry& e : entries) {
            put16(out, e.tag);
            put16(out, e.type);
            put32(out, e.count);
            put32(out, e.value);
        }
        nextIfdLink = out.size();
        put32(out, 0);
    }
    return out;
}

using Rgb = std::array<uint8_t, 3>;

// 3x2 (non-square, so a transpose would show) with a distinct colour per pixel.
const std::vector<Rgb> SOURCE_3X2 = {
    Rgb{255, 0, 0},   Rgb{0, 255, 0},   Rgb{0, 0, 255},     // top row
    Rgb{255, 255, 0}, Rgb{0, 255, 255}, Rgb{255, 0, 255},   // bottom row
};

std::vector<uint8_t> flatten(const std::vector<Rgb>& pixels) {
    std::vector<uint8_t> out;
    for (const Rgb& p : pixels) out.insert(out.end(), p.begin(), p.end());
    return out;
}

class ImageLoaderTiffTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path() /
               (std::string("qm_imageloader_") + info->name());
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path write(const std::string& name, const std::vector<uint8_t>& bytes) {
        const std::filesystem::path path = dir_ / name;
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return path;
    }

    // Binary PPM of the same pixels: stb_image reads it, so it pins the stb orientation.
    std::filesystem::path writePpm(const std::string& name, int w, int h,
                                   const std::vector<uint8_t>& rgb) {
        const std::string header = "P6\n" + std::to_string(w) + " " + std::to_string(h) +
                                   "\n255\n";
        std::vector<uint8_t> bytes(header.begin(), header.end());
        bytes.insert(bytes.end(), rgb.begin(), rgb.end());
        return write(name, bytes);
    }

    std::filesystem::path dir_;
};

TiffPage rgb8Page() {
    TiffPage page;
    page.width = 3;
    page.height = 2;
    page.data = flatten(SOURCE_3X2);
    return page;
}

// Pixel (x, y) of a loaded image, y counted in memory rows (row 0 = first stored).
const uint8_t* at(const ImageData& img, int x, int memoryRow) {
    return &img.pixels[(static_cast<size_t>(memoryRow) * img.width + x) * img.channels];
}

} // namespace

TEST_F(ImageLoaderTiffTest, Rgb8MatchesStbOrientationAndChannelOrder) {
    const auto tiffPath = write("rgb8.tif", buildTiff({rgb8Page()}));
    const auto ppmPath = writePpm("rgb8.ppm", 3, 2, flatten(SOURCE_3X2));

    auto tiff = ImageLoader::load(tiffPath);
    auto stb = ImageLoader::load(ppmPath);
    ASSERT_TRUE(tiff.isSuccess()) << tiff.error().message;
    ASSERT_TRUE(stb.isSuccess()) << stb.error().message;
    const ImageData& t = tiff.value();
    const ImageData& s = stb.value();

    EXPECT_EQ(t.width, 3);
    EXPECT_EQ(t.height, 2);
    EXPECT_EQ(t.channels, 4);
    EXPECT_EQ(t.pixels.size(), t.getDataSize());
    ASSERT_EQ(s.width, t.width);
    ASSERT_EQ(s.height, t.height);
    ASSERT_EQ(s.channels, 3);

    // Every pixel identical to the stb path, in the same memory position.
    for (int row = 0; row < t.height; ++row) {
        for (int x = 0; x < t.width; ++x) {
            const uint8_t* tp = at(t, x, row);
            const uint8_t* sp = at(s, x, row);
            EXPECT_EQ(tp[0], sp[0]) << "x=" << x << " row=" << row;
            EXPECT_EQ(tp[1], sp[1]) << "x=" << x << " row=" << row;
            EXPECT_EQ(tp[2], sp[2]) << "x=" << x << " row=" << row;
            EXPECT_EQ(tp[3], 255);
        }
    }

    // Both paths store the top row first, like video and procedural frames: the
    // source's top-left pixel (red) starts memory row 0, its bottom-left (yellow) the
    // last memory row.
    for (const ImageData* img : {&t, &s}) {
        const uint8_t* topLeft = at(*img, 0, 0);
        EXPECT_EQ((Rgb{topLeft[0], topLeft[1], topLeft[2]}), SOURCE_3X2[0]);
        const uint8_t* bottomLeft = at(*img, 0, img->height - 1);
        EXPECT_EQ((Rgb{bottomLeft[0], bottomLeft[1], bottomLeft[2]}), SOURCE_3X2[3]);
    }
}

TEST_F(ImageLoaderTiffTest, UppercaseExtensionAndTiffSpellingDecode) {
    const auto bytes = buildTiff({rgb8Page()});
    for (const char* name : {"a.TIF", "b.tiff", "c.TiFf"}) {
        auto result = ImageLoader::load(write(name, bytes));
        ASSERT_TRUE(result.isSuccess()) << name << ": " << result.error().message;
        EXPECT_EQ(result.value().width, 3) << name;
        EXPECT_EQ(result.value().height, 2) << name;
    }
}

TEST_F(ImageLoaderTiffTest, Rgba8KeepsStraightAlpha) {
    TiffPage page;
    page.width = 2;
    page.height = 2;
    page.samplesPerPixel = 4;
    page.unassociatedAlpha = true;
    page.data = {
        200, 10, 20, 255,   40, 50, 60, 128,
        70, 80, 90, 64,     1, 2, 3, 200,
    };
    auto result = ImageLoader::load(write("rgba8.tif", buildTiff({page})));
    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    const ImageData& img = result.value();
    ASSERT_EQ(img.channels, 4);

    // Source row 0 stays in memory row 0, channel order R, G, B, A.
    const uint8_t* p = at(img, 0, 0);
    EXPECT_EQ((std::array<uint8_t, 4>{p[0], p[1], p[2], p[3]}),
              (std::array<uint8_t, 4>{200, 10, 20, 255}));
    p = at(img, 1, 0);
    EXPECT_EQ((std::array<uint8_t, 4>{p[0], p[1], p[2], p[3]}),
              (std::array<uint8_t, 4>{40, 50, 60, 128}));
    p = at(img, 0, 1);
    EXPECT_EQ((std::array<uint8_t, 4>{p[0], p[1], p[2], p[3]}),
              (std::array<uint8_t, 4>{70, 80, 90, 64}));
    p = at(img, 1, 1);
    EXPECT_EQ((std::array<uint8_t, 4>{p[0], p[1], p[2], p[3]}),
              (std::array<uint8_t, 4>{1, 2, 3, 200}));
}

TEST_F(ImageLoaderTiffTest, Gray8ExpandsToOpaqueRgba) {
    TiffPage page;
    page.width = 2;
    page.height = 1;
    page.samplesPerPixel = 1;
    page.photometric = 1;
    page.data = {0x20, 0xD0};
    auto result = ImageLoader::load(write("gray8.tif", buildTiff({page})));
    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    const ImageData& img = result.value();
    ASSERT_EQ(img.width, 2);
    ASSERT_EQ(img.height, 1);
    ASSERT_EQ(img.channels, 4);
    const uint8_t expected[2] = {0x20, 0xD0};
    for (int x = 0; x < 2; ++x) {
        const uint8_t* p = at(img, x, 0);
        EXPECT_EQ(p[0], expected[x]);
        EXPECT_EQ(p[1], expected[x]);
        EXPECT_EQ(p[2], expected[x]);
        EXPECT_EQ(p[3], 255);
    }
}

TEST_F(ImageLoaderTiffTest, Gray16ConvertsTo8BitRgba) {
    TiffPage page;
    page.width = 2;
    page.height = 2;
    page.samplesPerPixel = 1;
    page.bitsPerSample = 16;
    page.photometric = 1;
    const uint16_t samples[4] = {0x0000, 0xFFFF, 0x8080, 0x4040};  // top row, bottom row
    for (uint16_t v : samples) put16(page.data, v);

    auto result = ImageLoader::load(write("gray16.tif", buildTiff({page})));
    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    const ImageData& img = result.value();
    ASSERT_EQ(img.width, 2);
    ASSERT_EQ(img.height, 2);
    ASSERT_EQ(img.channels, 4);

    // 16-bit v scales to v/257; top row sits in memory row 0.
    const uint8_t expected[4] = {0x00, 0xFF, 0x80, 0x40};
    const int memoryRow[4] = {0, 0, 1, 1};
    for (int i = 0; i < 4; ++i) {
        const uint8_t* p = at(img, i % 2, memoryRow[i]);
        EXPECT_NEAR(p[0], expected[i], 1) << "sample " << i;
        EXPECT_EQ(p[0], p[1]) << "sample " << i;
        EXPECT_EQ(p[0], p[2]) << "sample " << i;
        EXPECT_EQ(p[3], 255) << "sample " << i;
    }
}

TEST_F(ImageLoaderTiffTest, Rgb16ConvertsTo8BitRgba) {
    TiffPage page;
    page.width = 1;
    page.height = 2;
    page.bitsPerSample = 16;
    for (uint16_t v : {0xFFFF, 0x0000, 0x8080,     // top pixel
                       0x0000, 0xFFFF, 0x0000}) {  // bottom pixel
        put16(page.data, v);
    }
    auto result = ImageLoader::load(write("rgb16.tif", buildTiff({page})));
    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    const ImageData& img = result.value();
    ASSERT_EQ(img.width, 1);
    ASSERT_EQ(img.height, 2);
    ASSERT_EQ(img.channels, 4);
    const uint8_t* top = at(img, 0, 0);
    EXPECT_EQ(top[0], 255);
    EXPECT_EQ(top[1], 0);
    EXPECT_NEAR(top[2], 0x80, 1);
    EXPECT_EQ(top[3], 255);
    const uint8_t* bottom = at(img, 0, 1);
    EXPECT_EQ(bottom[0], 0);
    EXPECT_EQ(bottom[1], 255);
    EXPECT_EQ(bottom[2], 0);
}

TEST_F(ImageLoaderTiffTest, MultiPageLoadsFirstFrame) {
    TiffPage second;
    second.width = 5;
    second.height = 4;
    second.data.assign(5 * 4 * 3, 7);
    auto result = ImageLoader::load(write("multi.tif", buildTiff({rgb8Page(), second})));
    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    EXPECT_EQ(result.value().width, 3);
    EXPECT_EQ(result.value().height, 2);
    const uint8_t* topLeft = at(result.value(), 0, 0);
    EXPECT_EQ((Rgb{topLeft[0], topLeft[1], topLeft[2]}), SOURCE_3X2[0]);
}

TEST_F(ImageLoaderTiffTest, ForcedChannelCountsMatchStbSemantics) {
    const auto tiffPath = write("rgb8.tif", buildTiff({rgb8Page()}));
    const auto ppmPath = writePpm("rgb8.ppm", 3, 2, flatten(SOURCE_3X2));

    for (int channels = 1; channels <= 4; ++channels) {
        auto tiff = ImageLoader::loadWithChannels(tiffPath, channels);
        auto stb = ImageLoader::loadWithChannels(ppmPath, channels);
        ASSERT_TRUE(tiff.isSuccess()) << tiff.error().message;
        ASSERT_TRUE(stb.isSuccess()) << stb.error().message;
        EXPECT_EQ(tiff.value().channels, channels);
        EXPECT_EQ(tiff.value().pixels.size(), tiff.value().getDataSize());
        EXPECT_EQ(tiff.value().pixels, stb.value().pixels) << channels << " channels";
    }
}

TEST_F(ImageLoaderTiffTest, CorruptFileReturnsClearError) {
    const auto path = write("broken.tif", {'n', 'o', 't', ' ', 'a', ' ', 't', 'i', 'f', 'f'});
    auto result = ImageLoader::load(path);
    ASSERT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("TIFF"), std::string::npos) << result.error().message;
    EXPECT_NE(result.error().message.find("broken.tif"), std::string::npos)
        << result.error().message;
}

TEST_F(ImageLoaderTiffTest, TruncatedPixelDataReturnsError) {
    // Valid header and IFD, but the strip points past the end of the file.
    TiffPage page = rgb8Page();
    std::vector<uint8_t> bytes = buildTiff({page});
    // Strip offset entry (tag 273) value: rewrite it to far beyond EOF.
    for (size_t i = 8; i + 12 <= bytes.size(); i += 2) {
        if (bytes[i] == 0x11 && bytes[i + 1] == 0x01 && bytes[i + 2] == 4) {  // tag 273, LONG
            set32(bytes, i + 8, 0x00100000);
            break;
        }
    }
    auto result = ImageLoader::load(write("truncated.tif", bytes));
    EXPECT_TRUE(result.isError());
}

TEST_F(ImageLoaderTiffTest, EmptyFileReturnsError) {
    auto result = ImageLoader::load(write("empty.tiff", {}));
    EXPECT_TRUE(result.isError());
}

TEST_F(ImageLoaderTiffTest, MissingFileReturnsError) {
    auto result = ImageLoader::load(dir_ / "missing.tif");
    ASSERT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("not found"), std::string::npos);
}

// The header is checked before decoding: a 20000 x 20000 claim is refused for its size,
// not after stb has tried to allocate 1.2 GB for it.
TEST_F(ImageLoaderTiffTest, OversizedImageIsRefusedFromItsHeader) {
    const auto path = writePpm("huge.ppm", 20000, 20000, {});
    auto result = ImageLoader::load(path);
    ASSERT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("too large"), std::string::npos) << result.error().message;
}

// A folder named outside the ANSI code page: stb_image decodes from memory read
// through the wide path, WIC opens the wide name.
TEST_F(ImageLoaderTiffTest, LoadsFromNonAsciiFolder) {
    const std::filesystem::path folder = dir_ / pathFromUtf8(u8"\u00fc \u65e5\u672c \u0416");
    std::filesystem::create_directories(folder);
    auto put = [](const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    };
    const std::string header = "P6\n3 2\n255\n";
    std::vector<uint8_t> ppmBytes(header.begin(), header.end());
    const std::vector<uint8_t> rgb = flatten(SOURCE_3X2);
    ppmBytes.insert(ppmBytes.end(), rgb.begin(), rgb.end());
    const std::filesystem::path ppm = folder / pathFromUtf8(u8"b\u00fchne \u0416.ppm");
    const std::filesystem::path tiff = folder / pathFromUtf8(u8"\u65e5\u672c.tif");
    put(ppm, ppmBytes);
    put(tiff, buildTiff({rgb8Page()}));

    for (const auto& path : {ppm, tiff}) {
        auto result = ImageLoader::load(path);
        ASSERT_TRUE(result.isSuccess()) << result.error().message;
        EXPECT_EQ(result.value().width, 3);
        EXPECT_EQ(result.value().height, 2);
        auto rgba = ImageLoader::loadWithChannels(path, 4);
        ASSERT_TRUE(rgba.isSuccess()) << rgba.error().message;
        EXPECT_EQ(rgba.value().channels, 4);
    }
}

TEST_F(ImageLoaderTiffTest, DecodesOnFreshWorkerThread) {
    const auto path = write("rgb8.tif", buildTiff({rgb8Page()}));
    bool ok = false;
    std::thread worker([&] { ok = ImageLoader::load(path).isSuccess(); });
    worker.join();
    EXPECT_TRUE(ok);
}

TEST_F(ImageLoaderTiffTest, DecodesOnSingleThreadedApartmentAndLeavesItInitialised) {
    // A thread already in an STA makes our MTA init fail with RPC_E_CHANGED_MODE: the
    // decode must still work, and must not tear down the caller's COM.
    const auto path = write("rgb8.tif", buildTiff({rgb8Page()}));
    bool ok = false;
    HRESULT afterHr = E_FAIL;
    std::thread worker([&] {
        const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ok = SUCCEEDED(initHr) && ImageLoader::load(path).isSuccess();
        // S_FALSE = this thread is still in its STA (our scope did not uninitialise it).
        afterHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(afterHr)) CoUninitialize();
        if (SUCCEEDED(initHr)) CoUninitialize();
    });
    worker.join();
    EXPECT_TRUE(ok);
    EXPECT_EQ(afterHr, S_FALSE);
}

TEST_F(ImageLoaderTiffTest, RepeatedLoadsOnOneThreadBalanceComInit) {
    const auto path = write("rgb8.tif", buildTiff({rgb8Page()}));
    HRESULT afterHr = E_FAIL;
    bool ok = true;
    std::thread worker([&] {
        for (int i = 0; i < 20; ++i) ok = ok && ImageLoader::load(path).isSuccess();
        // COM fully released again: a fresh STA init succeeds with S_OK, not
        // RPC_E_CHANGED_MODE (which a leaked MTA reference would cause).
        afterHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(afterHr)) CoUninitialize();
    });
    worker.join();
    EXPECT_TRUE(ok);
    EXPECT_EQ(afterHr, S_OK);
}

TEST(ImageLoaderExtensionTest, TiffExtensionsAreSupportedEverywhere) {
    for (const char* ext : {".tif", ".tiff", ".TIF", ".Tiff"}) {
        EXPECT_TRUE(ImageLoader::isSupported(std::filesystem::path("a") += ext)) << ext;
        EXPECT_TRUE(MediaLibrary::isExtensionSupported(ext)) << ext;
        EXPECT_EQ(MediaLibrary::getTypeFromExtension(ext), MediaType::IMAGE) << ext;
    }
    const auto& images = MediaLibrary::getSupportedImageExtensions();
    EXPECT_NE(std::find(images.begin(), images.end(), ".tif"), images.end());
    EXPECT_NE(std::find(images.begin(), images.end(), ".tiff"), images.end());
}

// Every format the loader decodes is classed as an image by the media library, so
// relocation, drag-drop and import accept it (TD-24 removed the IMAGE fallback).
TEST(ImageLoaderExtensionTest, EveryLoaderFormatIsAMediaLibraryImage) {
    for (const char* ext : {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga", ".psd", ".hdr",
                            ".pic", ".ppm", ".pgm", ".tif", ".tiff"}) {
        ASSERT_TRUE(ImageLoader::isSupported(std::filesystem::path("a") += ext)) << ext;
        EXPECT_EQ(MediaLibrary::getTypeFromExtension(ext), MediaType::IMAGE) << ext;
    }
    for (const std::string& ext : MediaLibrary::getSupportedImageExtensions()) {
        EXPECT_TRUE(ImageLoader::isSupported(std::filesystem::path("a") += ext)) << ext;
    }
}

// ---- Grey images come out as RGBA (a 1- or 2-channel texture would display red) ----

namespace {

uint32_t crc32Of(const std::vector<uint8_t>& bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : bytes) {
        crc ^= b;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void putBe32(std::vector<uint8_t>& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

void putChunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    putBe32(png, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    putBe32(png, crc32Of(body));
}

// An 8-bit PNG (colour type 0 grey or 4 grey+alpha) with stored (uncompressed) deflate.
std::vector<uint8_t> makeGreyPng(uint32_t w, uint32_t h, uint8_t colorType,
                                 const std::vector<uint8_t>& samples) {
    const size_t stride = colorType == 4 ? 2 : 1;
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), samples.begin() + y * w * stride, samples.begin() + (y + 1) * w * stride);
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521u; b = (b + a) % 65521u; }
    std::vector<uint8_t> z = {0x78, 0x01, 0x01};
    const uint16_t len = static_cast<uint16_t>(raw.size());
    z.push_back(static_cast<uint8_t>(len & 0xFF));
    z.push_back(static_cast<uint8_t>(len >> 8));
    z.push_back(static_cast<uint8_t>(~len & 0xFF));
    z.push_back(static_cast<uint8_t>((~len) >> 8));
    z.insert(z.end(), raw.begin(), raw.end());
    putBe32(z, (b << 16) | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    putBe32(ihdr, w);
    putBe32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, colorType, 0, 0, 0});
    putChunk(png, "IHDR", ihdr);
    putChunk(png, "IDAT", z);
    putChunk(png, "IEND", {});
    return png;
}

}  // namespace

TEST_F(ImageLoaderTiffTest, OneChannelPngLoadsAsOpaqueRgbaWithGreyInRgb) {
    const auto path = write("grey.png", makeGreyPng(2, 2, 0, {0, 64, 128, 255}));
    for (auto result : {ImageLoader::load(path), ImageLoader::loadWithChannels(path, 0)}) {
        ASSERT_TRUE(result.isSuccess()) << result.error().message;
        const ImageData& img = result.value();
        ASSERT_EQ(img.channels, 4);
        ASSERT_EQ(img.pixels.size(), 2u * 2u * 4u);
        const uint8_t grey[4] = {0, 64, 128, 255};
        for (int i = 0; i < 4; ++i) {
            EXPECT_EQ(img.pixels[i * 4 + 0], grey[i]);
            EXPECT_EQ(img.pixels[i * 4 + 1], grey[i]);
            EXPECT_EQ(img.pixels[i * 4 + 2], grey[i]);
            EXPECT_EQ(img.pixels[i * 4 + 3], 255);
        }
    }
}

TEST_F(ImageLoaderTiffTest, TwoChannelPngLoadsAsRgbaKeepingItsAlpha) {
    const auto path = write("grey_alpha.png", makeGreyPng(2, 1, 4, {10, 200, 90, 30}));
    auto result = ImageLoader::load(path);
    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    const ImageData& img = result.value();
    ASSERT_EQ(img.channels, 4);
    ASSERT_EQ(img.pixels.size(), 8u);
    const std::vector<uint8_t> expected = {10, 10, 10, 200, 90, 90, 90, 30};
    EXPECT_EQ(img.pixels, expected);
}

} // namespace test
} // namespace quadmapper
