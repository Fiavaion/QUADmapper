// Output slices and edge-blend maths (ADR-047).
#include <gtest/gtest.h>
#include "scene/Slice.hpp"

#include <cmath>

using namespace quadmapper;

TEST(SliceTest, FullCanvasSliceIsUnwarpedAndUnblended) {
    const Slice slice = makeFullCanvasSlice(IVec2(1280, 800));
    EXPECT_FALSE(slice.id.isNull());
    EXPECT_EQ(slice.input, (CanvasRect{0.0f, 0.0f, 1280.0f, 800.0f}));
    EXPECT_TRUE(isIdentityWarp(slice.warp));
    for (const BlendEdge& edge : slice.blend) EXPECT_EQ(edge.width, 0.0f);
    EXPECT_EQ(slice.blackLevel, 0.0f);
    EXPECT_NE(makeFullCanvasSlice(IVec2(1, 1)).id, slice.id);
}

TEST(SliceTest, IdentityWarpIsOnlyTheWholeUnchangedFrame) {
    QuadGeometry warp = fullFrameWarp();
    EXPECT_TRUE(isIdentityWarp(warp));
    warp.setMeshMode(MeshMode::MESH_EDIT);
    warp.setUniformGrid(3);
    EXPECT_TRUE(isIdentityWarp(warp)) << "a grid with nothing moved is still the frame";
    warp.setMeshVertex(1, 1, Vec2(0.01f, 0.0f));
    EXPECT_FALSE(isIdentityWarp(warp));
    QuadGeometry keystone = fullFrameWarp();
    keystone.setCorner(1, Vec2(0.95f, 0.02f));
    EXPECT_FALSE(isIdentityWarp(keystone));
}

TEST(SliceTest, RescaleKeepsSlicesAtTheSamePlaceOnThePicture) {
    std::vector<Slice> slices{makeFullCanvasSlice(IVec2(1920, 1080))};
    slices[0].input = CanvasRect{960.0f, 270.0f, 960.0f, 540.0f};
    slices[0].blend[SLICE_EDGE_LEFT].width = 96.0f;
    slices[0].blend[SLICE_EDGE_TOP].width = 54.0f;
    rescaleSlices(slices, IVec2(1920, 1080), IVec2(3840, 1080));
    EXPECT_EQ(slices[0].input, (CanvasRect{1920.0f, 270.0f, 1920.0f, 540.0f}));
    EXPECT_FLOAT_EQ(slices[0].blend[SLICE_EDGE_LEFT].width, 192.0f);
    EXPECT_FLOAT_EQ(slices[0].blend[SLICE_EDGE_TOP].width, 54.0f);
    rescaleSlices(slices, IVec2(0, 0), IVec2(10, 10));  // no known size: unchanged
    EXPECT_EQ(slices[0].input, (CanvasRect{1920.0f, 270.0f, 1920.0f, 540.0f}));
}

// A slice of the whole canvas stays exactly the whole canvas through any rescale, so
// it keeps the exact-copy path and the lone-output canvas keeps following (800 ->
// 1920 in float was 1920.0001).
TEST(SliceTest, RescaleOfTheWholeCanvasIsExactlyTheWholeCanvas) {
    const int sizes[] = {480, 540, 600, 720, 768, 800, 900, 1024, 1050, 1080, 1200, 1280,
                         1360, 1366, 1440, 1600, 1680, 1920, 2048, 2160, 2560, 3840};
    for (int a : sizes) {
        for (int b : sizes) {
            // Every pair, on both axes: a x b -> b x a.
            std::vector<Slice> slices{makeFullCanvasSlice(IVec2(a, b))};
            rescaleSlices(slices, IVec2(a, b), IVec2(b, a));
            EXPECT_TRUE(isDefaultSlicing(slices, IVec2(b, a)))
                << a << "x" << b << " -> " << b << "x" << a << ": " << slices[0].input.width << " x "
                << slices[0].input.height;
        }
    }
    // Edges that land on whole pixels are exact too.
    std::vector<Slice> half{makeFullCanvasSlice(IVec2(800, 600))};
    half[0].input = CanvasRect{400.0f, 0.0f, 400.0f, 600.0f};
    rescaleSlices(half, IVec2(800, 600), IVec2(1920, 1080));
    EXPECT_EQ(half[0].input, (CanvasRect{960.0f, 0.0f, 960.0f, 1080.0f}));
}

TEST(SliceTest, DefaultSlicingIsOneUntouchedWholeCanvasSlice) {
    const IVec2 canvas(1280, 800);
    std::vector<Slice> slices{makeFullCanvasSlice(canvas)};
    EXPECT_TRUE(isDefaultSlicing(slices, canvas));
    EXPECT_FALSE(isDefaultSlicing(slices, IVec2(1920, 1080)));
    EXPECT_FALSE(isDefaultSlicing({}, canvas));
    slices[0].blackLevel = 0.01f;
    EXPECT_FALSE(isDefaultSlicing(slices, canvas));
    slices[0].blackLevel = 0.0f;
    slices[0].blend[SLICE_EDGE_BOTTOM].width = 1.0f;
    EXPECT_FALSE(isDefaultSlicing(slices, canvas));
    slices[0].blend[SLICE_EDGE_BOTTOM].width = 0.0f;
    slices.push_back(makeFullCanvasSlice(canvas));
    EXPECT_FALSE(isDefaultSlicing(slices, canvas));
}

// The ramp's defining property: two facing ramps add to one at every point, for any
// curve, so two projectors' light sums flat across the overlap.
TEST(SliceTest, FacingRampsSumToOneForEveryCurve) {
    for (float curve : {0.5f, 1.0f, 2.0f, 3.5f}) {
        EXPECT_FLOAT_EQ(blendRamp(0.0f, curve), 0.0f);
        EXPECT_FLOAT_EQ(blendRamp(0.5f, curve), 0.5f);
        EXPECT_FLOAT_EQ(blendRamp(1.0f, curve), 1.0f);
        float previous = -1.0f;
        for (int i = 0; i <= 100; ++i) {
            const float t = static_cast<float>(i) / 100.0f;
            EXPECT_NEAR(blendRamp(t, curve) + blendRamp(1.0f - t, curve), 1.0f, 1e-6f) << curve << " " << t;
            EXPECT_GE(blendRamp(t, curve), previous);  // monotonic
            previous = blendRamp(t, curve);
        }
    }
    EXPECT_FLOAT_EQ(blendRamp(-1.0f, 2.0f), 0.0f);  // clamped
    EXPECT_FLOAT_EQ(blendRamp(2.0f, 2.0f), 1.0f);
}

// Gate b (ADR-047 §8), on the maths: two outputs whose slices overlap by W canvas
// pixels, facing zones of width W, gamma 2.2. What a projector emits is its pixel
// value to the power gamma, so the sum of both projectors' light for full-white
// content is flat within 2% across the overlap (here it is exact up to float error).
TEST(SliceTest, TwoOutputOverlapSumsToFlatLinearLight) {
    const float overlap = 640.0f;
    Slice left = makeFullCanvasSlice(IVec2(1920, 1080));
    Slice right = makeFullCanvasSlice(IVec2(1920, 1080));
    right.input.x = 1920.0f - overlap;  // canvas 3200 wide
    left.blend[SLICE_EDGE_RIGHT] = BlendEdge{overlap, 2.2f, 2.0f};
    right.blend[SLICE_EDGE_LEFT] = BlendEdge{overlap, 2.2f, 2.0f};

    for (float x = right.input.x; x <= 1920.0f; x += 1.0f) {
        const float y = 540.0f;
        const float a = sliceBlendGain(left, Vec2(x - left.input.x, y - left.input.y));
        const float b = sliceBlendGain(right, Vec2(x - right.input.x, y - right.input.y));
        const float light = std::pow(a, 2.2f) + std::pow(b, 2.2f);
        EXPECT_NEAR(light, 1.0f, 0.02f) << "canvas x " << x;
        EXPECT_NEAR(light, 1.0f, 1e-4f) << "canvas x " << x;
    }
    // Outside the overlap each projector is alone at full gain.
    EXPECT_FLOAT_EQ(sliceBlendGain(left, Vec2(100.0f, 540.0f)), 1.0f);
    EXPECT_FLOAT_EQ(sliceBlendGain(right, Vec2(1900.0f, 540.0f)), 1.0f);
    // And in pixel values the zone is gamma-shaped, not linear: halfway the gain is
    // 0.5^(1/2.2), about 0.73.
    EXPECT_NEAR(sliceBlendGain(left, Vec2(1920.0f - overlap / 2.0f, 540.0f)),
                std::pow(0.5f, 1.0f / 2.2f), 1e-5f);
}

TEST(SliceTest, BlackLevelAppliesOnlyOutsideTheBlendZones) {
    Slice slice = makeFullCanvasSlice(IVec2(1000, 500));
    slice.blend[SLICE_EDGE_RIGHT].width = 100.0f;
    slice.blend[SLICE_EDGE_BOTTOM].width = 50.0f;
    EXPECT_TRUE(outsideBlendZones(slice, Vec2(500.0f, 200.0f)));
    EXPECT_FALSE(outsideBlendZones(slice, Vec2(950.0f, 200.0f)));
    EXPECT_FALSE(outsideBlendZones(slice, Vec2(500.0f, 480.0f)));
    EXPECT_TRUE(outsideBlendZones(slice, Vec2(899.0f, 449.0f)));
}

// ---- Auto-blend pairing (ADR-047 §3) ------------------------------------------------

namespace {

// Four outputs' ids, made on first use (UUID::generate is not usable during static
// initialization of this test file).
const UUID& outputId(int i) {
    static const UUID ids[4] = {UUID::generate(), UUID::generate(), UUID::generate(), UUID::generate()};
    return ids[i];
}

// The width auto-blend gives edge `edge` of slice `slice`, or -1 when it sets none.
float widthOf(const std::vector<AutoBlendEdge>& edges, size_t slice, SliceEdge edge) {
    for (const AutoBlendEdge& e : edges) {
        if (e.slice == slice && e.edge == edge) return e.width;
    }
    return -1.0f;
}

}  // namespace

TEST(AutoBlendTest, HorizontalPairBlendsTheFacingLeftAndRightEdges) {
    const std::vector<SlicePlacement> slices{{outputId(0), CanvasRect{0.0f, 0.0f, 1920.0f, 1080.0f}},
                                             {outputId(1), CanvasRect{1280.0f, 0.0f, 1920.0f, 1080.0f}}};
    const auto edges = autoBlendEdges(slices);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_FLOAT_EQ(widthOf(edges, 0, SLICE_EDGE_RIGHT), 640.0f);
    EXPECT_FLOAT_EQ(widthOf(edges, 1, SLICE_EDGE_LEFT), 640.0f);
    // The order of the slices does not matter.
    const auto swapped = autoBlendEdges({slices[1], slices[0]});
    EXPECT_FLOAT_EQ(widthOf(swapped, 1, SLICE_EDGE_RIGHT), 640.0f);
    EXPECT_FLOAT_EQ(widthOf(swapped, 0, SLICE_EDGE_LEFT), 640.0f);
}

TEST(AutoBlendTest, VerticalPairBlendsTheFacingTopAndBottomEdges) {
    const auto edges = autoBlendEdges({{outputId(0), CanvasRect{0.0f, 0.0f, 1920.0f, 1080.0f}},
                                       {outputId(1), CanvasRect{0.0f, 880.0f, 1920.0f, 1080.0f}}});
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_FLOAT_EQ(widthOf(edges, 0, SLICE_EDGE_BOTTOM), 200.0f);
    EXPECT_FLOAT_EQ(widthOf(edges, 1, SLICE_EDGE_TOP), 200.0f);
}

// A 2x2 grid: each slice blends with its right/left and top/bottom neighbour; the
// diagonal pairs, which share only a corner patch, add nothing.
TEST(AutoBlendTest, TwoByTwoGridBlendsEachSliceWithItsTwoNeighbours) {
    const std::vector<SlicePlacement> slices{
        {outputId(0), CanvasRect{0.0f, 0.0f, 1920.0f, 1080.0f}},      // top left
        {outputId(1), CanvasRect{1600.0f, 0.0f, 1920.0f, 1080.0f}},   // top right
        {outputId(2), CanvasRect{0.0f, 900.0f, 1920.0f, 1080.0f}},    // bottom left
        {outputId(3), CanvasRect{1600.0f, 900.0f, 1920.0f, 1080.0f}}  // bottom right
    };
    const auto edges = autoBlendEdges(slices);
    EXPECT_EQ(edges.size(), 8u);
    EXPECT_FLOAT_EQ(widthOf(edges, 0, SLICE_EDGE_RIGHT), 320.0f);
    EXPECT_FLOAT_EQ(widthOf(edges, 0, SLICE_EDGE_BOTTOM), 180.0f);
    EXPECT_FLOAT_EQ(widthOf(edges, 1, SLICE_EDGE_LEFT), 320.0f);
    EXPECT_FLOAT_EQ(widthOf(edges, 1, SLICE_EDGE_BOTTOM), 180.0f);
    EXPECT_FLOAT_EQ(widthOf(edges, 2, SLICE_EDGE_RIGHT), 320.0f);
    EXPECT_FLOAT_EQ(widthOf(edges, 2, SLICE_EDGE_TOP), 180.0f);
    EXPECT_FLOAT_EQ(widthOf(edges, 3, SLICE_EDGE_LEFT), 320.0f);
    EXPECT_FLOAT_EQ(widthOf(edges, 3, SLICE_EDGE_TOP), 180.0f);
    EXPECT_LT(widthOf(edges, 0, SLICE_EDGE_LEFT), 0.0f);
    EXPECT_LT(widthOf(edges, 0, SLICE_EDGE_TOP), 0.0f);
}

TEST(AutoBlendTest, NonOverlappingSlicesGetNoBlend) {
    EXPECT_TRUE(autoBlendEdges({{outputId(0), CanvasRect{0.0f, 0.0f, 1920.0f, 1080.0f}},
                                {outputId(1), CanvasRect{1920.0f, 0.0f, 1920.0f, 1080.0f}}})
                    .empty())
        << "touching edges do not overlap";
    EXPECT_TRUE(autoBlendEdges({{outputId(0), CanvasRect{0.0f, 0.0f, 1000.0f, 1000.0f}},
                                {outputId(1), CanvasRect{2000.0f, 1500.0f, 1000.0f, 1000.0f}}})
                    .empty());
    EXPECT_TRUE(autoBlendEdges({{outputId(0), CanvasRect{0.0f, 0.0f, 1920.0f, 1080.0f}}}).empty());
}

// Side by side, with the right projector's slice lower by 100 px: still a left/right
// pair, blended over the overlap's width.
TEST(AutoBlendTest, PartialVerticalOffsetIsStillALeftRightPair) {
    const auto edges = autoBlendEdges({{outputId(0), CanvasRect{0.0f, 0.0f, 1920.0f, 1080.0f}},
                                       {outputId(1), CanvasRect{1600.0f, 100.0f, 1920.0f, 1080.0f}}});
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_FLOAT_EQ(widthOf(edges, 0, SLICE_EDGE_RIGHT), 320.0f);
    EXPECT_FLOAT_EQ(widthOf(edges, 1, SLICE_EDGE_LEFT), 320.0f);
}

TEST(AutoBlendTest, SlicesOfOneOutputDoNotBlendWithEachOther) {
    EXPECT_TRUE(autoBlendEdges({{outputId(0), CanvasRect{0.0f, 0.0f, 1920.0f, 1080.0f}},
                                {outputId(0), CanvasRect{1280.0f, 0.0f, 1920.0f, 1080.0f}}})
                    .empty());
}
