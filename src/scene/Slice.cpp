#include "Slice.hpp"

#include <algorithm>
#include <cmath>

namespace quadmapper {

QuadGeometry fullFrameWarp() {
    QuadGeometry warp;
    warp.setCorners({Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)});
    return warp;
}

Slice makeFullCanvasSlice(IVec2 canvasSize) {
    Slice slice;
    slice.id = UUID::generate();
    slice.input = CanvasRect{0.0f, 0.0f, static_cast<float>(canvasSize.x),
                             static_cast<float>(canvasSize.y)};
    return slice;
}

bool isIdentityWarp(const QuadGeometry& warp) {
    const auto& c = warp.getCorners();
    return c[0] == Vec2(0.0f, 0.0f) && c[1] == Vec2(1.0f, 0.0f) && c[2] == Vec2(1.0f, 1.0f) &&
           c[3] == Vec2(0.0f, 1.0f) && !warp.hasResidualSurface() && !warp.hasBezierEdges();
}

void rescaleSlices(std::vector<Slice>& slices, IVec2 from, IVec2 to) {
    if (from.x <= 0 || from.y <= 0 || to.x <= 0 || to.y <= 0 || from == to) return;
    const float sx = static_cast<float>(to.x) / static_cast<float>(from.x);
    const float sy = static_cast<float>(to.y) / static_cast<float>(from.y);
    for (Slice& slice : slices) {
        slice.input.x *= sx;
        slice.input.y *= sy;
        slice.input.width *= sx;
        slice.input.height *= sy;
        slice.blend[SLICE_EDGE_LEFT].width *= sx;
        slice.blend[SLICE_EDGE_RIGHT].width *= sx;
        slice.blend[SLICE_EDGE_TOP].width *= sy;
        slice.blend[SLICE_EDGE_BOTTOM].width *= sy;
    }
}

float blendRamp(float t, float curve) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (t < 0.5f) return 0.5f * std::pow(2.0f * t, curve);
    return 1.0f - 0.5f * std::pow(2.0f * (1.0f - t), curve);
}

float blendEdgeGain(const BlendEdge& edge, float distance) {
    if (edge.width <= 0.0f || distance >= edge.width) return 1.0f;
    return std::pow(blendRamp(distance / edge.width, edge.curve), 1.0f / edge.gamma);
}

namespace {

// Distance of a point inside the slice from each edge, in SliceEdge order.
std::array<float, SLICE_EDGE_COUNT> edgeDistances(const Slice& slice, Vec2 p) {
    return {p.x, slice.input.width - p.x, p.y, slice.input.height - p.y};
}

}  // namespace

float sliceBlendGain(const Slice& slice, Vec2 inSlice) {
    const auto d = edgeDistances(slice, inSlice);
    float gain = 1.0f;
    for (int e = 0; e < SLICE_EDGE_COUNT; ++e) {
        gain *= blendEdgeGain(slice.blend[static_cast<size_t>(e)], d[static_cast<size_t>(e)]);
    }
    return gain;
}

bool outsideBlendZones(const Slice& slice, Vec2 inSlice) {
    const auto d = edgeDistances(slice, inSlice);
    for (int e = 0; e < SLICE_EDGE_COUNT; ++e) {
        if (d[static_cast<size_t>(e)] < slice.blend[static_cast<size_t>(e)].width) return false;
    }
    return true;
}

} // namespace quadmapper
