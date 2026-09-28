#include <gtest/gtest.h>
#include "scene/CropInsets.hpp"

#include <array>

namespace quadmapper {
namespace test {

namespace {

constexpr std::array<CropEdge, 4> ALL_EDGES = {CropEdge::Left, CropEdge::Top,
                                               CropEdge::Right, CropEdge::Bottom};

std::array<int, 4> pixels(const NormalizedRect& crop, int w, int h) {
    return {cropInsetPixels(crop, CropEdge::Left, w, h),
            cropInsetPixels(crop, CropEdge::Top, w, h),
            cropInsetPixels(crop, CropEdge::Right, w, h),
            cropInsetPixels(crop, CropEdge::Bottom, w, h)};
}

void expectNotInverted(const NormalizedRect& c) {
    EXPECT_GE(c.x, 0.0f);
    EXPECT_GE(c.y, 0.0f);
    EXPECT_GT(c.width, 0.0f);
    EXPECT_GT(c.height, 0.0f);
    EXPECT_LE(c.x + c.width, 1.0f + 1e-6f);
    EXPECT_LE(c.y + c.height, 1.0f + 1e-6f);
}

} // namespace

TEST(CropInsetsTest, IdentityCropHasZeroInsets) {
    const NormalizedRect full;
    for (CropEdge e : ALL_EDGES) {
        EXPECT_DOUBLE_EQ(cropInset(full, e), 0.0);
        EXPECT_EQ(cropInsetPixels(full, e, 1920, 1080), 0);
    }
}

TEST(CropInsetsTest, InsetsReadFromStoredRect) {
    const NormalizedRect crop{0.25f, 0.125f, 0.5f, 0.75f};
    EXPECT_NEAR(cropInset(crop, CropEdge::Left), 0.25, 1e-7);
    EXPECT_NEAR(cropInset(crop, CropEdge::Right), 0.25, 1e-7);
    EXPECT_NEAR(cropInset(crop, CropEdge::Top), 0.125, 1e-7);
    EXPECT_NEAR(cropInset(crop, CropEdge::Bottom), 0.125, 1e-7);
    EXPECT_EQ(pixels(crop, 1920, 1080), (std::array<int, 4>{480, 135, 480, 135}));
}

TEST(CropInsetsTest, PixelEditStoresExactFractionAndReadsBack) {
    NormalizedRect crop;
    crop = withCropInsetPixels(crop, CropEdge::Left, 100, 1920, 1080);
    crop = withCropInsetPixels(crop, CropEdge::Top, 33, 1920, 1080);
    crop = withCropInsetPixels(crop, CropEdge::Right, 7, 1920, 1080);
    crop = withCropInsetPixels(crop, CropEdge::Bottom, 1, 1920, 1080);
    EXPECT_EQ(pixels(crop, 1920, 1080), (std::array<int, 4>{100, 33, 7, 1}));
    EXPECT_FLOAT_EQ(crop.x, 100.0f / 1920.0f);
    EXPECT_FLOAT_EQ(crop.y, 33.0f / 1080.0f);
    EXPECT_NEAR(crop.width, 1.0 - 107.0 / 1920.0, 1e-7);
    EXPECT_NEAR(crop.height, 1.0 - 34.0 / 1080.0, 1e-7);
}

TEST(CropInsetsTest, EveryPixelValueRoundTripsOnOddSizes) {
    for (int size : {1, 2, 3, 5, 7, 99, 1001, 1919, 4097, 16383}) {
        for (int px = 0; px < size; ++px) {
            if (size > 200 && px % 37 != 0 && px != size - 1) continue;  // keep it quick
            for (CropEdge e : ALL_EDGES) {
                const NormalizedRect c = withCropInsetPixels(NormalizedRect{}, e, px, size, size);
                ASSERT_EQ(cropInsetPixels(c, e, size, size), px)
                    << "size=" << size << " px=" << px << " edge=" << static_cast<int>(e);
                expectNotInverted(c);
            }
        }
    }
}

TEST(CropInsetsTest, RepeatedEditsDoNotDriftUntouchedEdges) {
    const int w = 1921, h = 1079;  // odd, not a power of two
    NormalizedRect crop;
    crop = withCropInsetPixels(crop, CropEdge::Right, 311, w, h);
    crop = withCropInsetPixels(crop, CropEdge::Bottom, 97, w, h);
    const NormalizedRect before = crop;

    // Drag the left and top edges back and forth thousands of times.
    for (int i = 0; i < 5000; ++i) {
        crop = withCropInsetPixels(crop, CropEdge::Left, i % 400, w, h);
        crop = withCropInsetPixels(crop, CropEdge::Top, (i * 7) % 300, w, h);
    }
    crop = withCropInsetPixels(crop, CropEdge::Left, 0, w, h);
    crop = withCropInsetPixels(crop, CropEdge::Top, 0, w, h);

    EXPECT_EQ(pixels(crop, w, h), (std::array<int, 4>{0, 0, 311, 97}));
    EXPECT_NEAR(crop.width, before.width, 1e-6);
    EXPECT_NEAR(crop.height, before.height, 1e-6);
    EXPECT_NEAR(cropInset(crop, CropEdge::Right), 311.0 / w, 1e-6);
    EXPECT_NEAR(cropInset(crop, CropEdge::Bottom), 97.0 / h, 1e-6);
}

TEST(CropInsetsTest, RewritingTheDisplayedValueIsANoOp) {
    // What the inspector does every frame an edge is touched but not moved.
    const int w = 1280, h = 720;
    NormalizedRect crop{0.1f, 0.2f, 0.55f, 0.6f};
    const auto shown = pixels(crop, w, h);
    for (int i = 0; i < 1000; ++i) {
        for (size_t e = 0; e < ALL_EDGES.size(); ++e) {
            crop = withCropInsetPixels(crop, ALL_EDGES[e], shown[e], w, h);
        }
    }
    EXPECT_EQ(pixels(crop, w, h), shown);
}

TEST(CropInsetsTest, PixelInsetClampsToLeaveOnePixel) {
    const int w = 100, h = 50;
    NormalizedRect crop = withCropInsetPixels(NormalizedRect{}, CropEdge::Right, 30, w, h);
    EXPECT_EQ(maxCropInsetPixels(crop, CropEdge::Left, w, h), 69);

    crop = withCropInsetPixels(crop, CropEdge::Left, 500, w, h);
    EXPECT_EQ(cropInsetPixels(crop, CropEdge::Left, w, h), 69);
    EXPECT_EQ(cropInsetPixels(crop, CropEdge::Right, w, h), 30);  // opposite untouched
    EXPECT_NEAR(crop.width * w, 1.0, 1e-4);                       // one pixel remains
    expectNotInverted(crop);

    crop = withCropInsetPixels(crop, CropEdge::Top, -20, w, h);
    EXPECT_EQ(cropInsetPixels(crop, CropEdge::Top, w, h), 0);

    crop = withCropInsetPixels(crop, CropEdge::Bottom, 49, w, h);
    EXPECT_EQ(cropInsetPixels(crop, CropEdge::Bottom, w, h), 49);
    crop = withCropInsetPixels(crop, CropEdge::Top, 1, w, h);
    EXPECT_EQ(cropInsetPixels(crop, CropEdge::Top, w, h), 0);  // no room left
    expectNotInverted(crop);
}

TEST(CropInsetsTest, MaxIsExactAtLargeSizesDespiteFloatStorage) {
    for (int size : {4096, 8191, 16384}) {
        NormalizedRect crop = withCropInsetPixels(NormalizedRect{}, CropEdge::Left, 1234, size, size);
        EXPECT_EQ(maxCropInsetPixels(crop, CropEdge::Right, size, size), size - 1234 - 1);
        crop = withCropInsetPixels(crop, CropEdge::Right, size, size, size);
        EXPECT_EQ(cropInsetPixels(crop, CropEdge::Right, size, size), size - 1235);
        expectNotInverted(crop);
    }
}

TEST(CropInsetsTest, NonPixelAlignedCropDisplaysNearestAndKeepsOtherEdgesExact) {
    // "Left 1/2" on a 5 px wide source: 2.5 px, not a whole pixel.
    const int w = 5, h = 3;
    NormalizedRect crop{0.0f, 0.0f, 0.5f, 1.0f};
    EXPECT_EQ(cropInsetPixels(crop, CropEdge::Right, w, h), 3);  // lround(2.5)
    EXPECT_EQ(maxCropInsetPixels(crop, CropEdge::Left, w, h), 1);

    crop = withCropInsetPixels(crop, CropEdge::Top, 1, w, h);
    EXPECT_FLOAT_EQ(crop.x, 0.0f);
    EXPECT_FLOAT_EQ(crop.width, 0.5f);  // the horizontal axis is not snapped
}

TEST(CropInsetsTest, FractionEditMatchesLegacyPercentClamp) {
    // Percent fallback keeps the old rule: a 1% region always remains.
    NormalizedRect crop = withCropInset(NormalizedRect{}, CropEdge::Right, 0.4, 0.01);
    crop = withCropInset(crop, CropEdge::Left, 0.9, 0.01);
    EXPECT_NEAR(cropInset(crop, CropEdge::Left), 0.59, 1e-6);
    EXPECT_NEAR(crop.width, 0.01, 1e-6);

    crop = withCropInset(crop, CropEdge::Top, -1.0, 0.01);
    EXPECT_DOUBLE_EQ(cropInset(crop, CropEdge::Top), 0.0);
    crop = withCropInset(crop, CropEdge::Bottom, 2.0, 0.01);
    EXPECT_NEAR(cropInset(crop, CropEdge::Bottom), 0.99, 1e-6);
    expectNotInverted(crop);
}

TEST(CropInsetsTest, OutOfRangeStoredCropIsClampedAndNeverInverts) {
    // A corrupt or hand-edited crop: negative origin and overshooting size.
    NormalizedRect crop{-0.2f, 0.3f, 1.5f, 0.9f};
    EXPECT_DOUBLE_EQ(cropInset(crop, CropEdge::Left), 0.0);
    EXPECT_DOUBLE_EQ(cropInset(crop, CropEdge::Right), 0.0);
    EXPECT_DOUBLE_EQ(cropInset(crop, CropEdge::Bottom), 0.0);
    crop = withCropInsetPixels(crop, CropEdge::Left, 10, 100, 100);
    EXPECT_EQ(cropInsetPixels(crop, CropEdge::Left, 100, 100), 10);
    expectNotInverted(crop);
}

TEST(CropInsetsTest, UnknownSizeLeavesCropUnchanged) {
    const NormalizedRect crop{0.1f, 0.1f, 0.5f, 0.5f};
    const NormalizedRect out = withCropInsetPixels(crop, CropEdge::Left, 5, 0, 0);
    EXPECT_FLOAT_EQ(out.x, crop.x);
    EXPECT_FLOAT_EQ(out.width, crop.width);
    EXPECT_EQ(cropInsetPixels(crop, CropEdge::Left, 0, 0), 0);
    EXPECT_EQ(maxCropInsetPixels(crop, CropEdge::Left, 0, 0), 0);
}

TEST(CropInsetsTest, EdgesMeasureAlongTheirOwnAxis) {
    // Non-square source: left/right use width, top/bottom use height.
    const NormalizedRect crop{0.5f, 0.5f, 0.5f, 0.5f};
    EXPECT_EQ(cropInsetPixels(crop, CropEdge::Left, 200, 10), 100);
    EXPECT_EQ(cropInsetPixels(crop, CropEdge::Top, 200, 10), 5);
    EXPECT_EQ(maxCropInsetPixels(crop, CropEdge::Right, 200, 10), 99);
    EXPECT_EQ(maxCropInsetPixels(crop, CropEdge::Bottom, 200, 10), 4);
}

} // namespace test
} // namespace quadmapper
