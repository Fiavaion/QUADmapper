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
