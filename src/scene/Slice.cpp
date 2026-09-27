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

bool isDefaultSlicing(const std::vector<Slice>& slices, IVec2 canvas) {
    if (slices.size() != 1) return false;
    const Slice& only = slices.front();
    const CanvasRect whole{0.0f, 0.0f, static_cast<float>(canvas.x), static_cast<float>(canvas.y)};
    return only.input == whole && isIdentityWarp(only.warp) &&
           only.blend == std::array<BlendEdge, SLICE_EDGE_COUNT>{} && only.blackLevel == 0.0f;
}

void rescaleSlices(std::vector<Slice>& slices, IVec2 from, IVec2 to) {
    if (from.x <= 0 || from.y <= 0 || to.x <= 0 || to.y <= 0 || from == to) return;
    // In double, value * to / from is exact for whole pixels (800 -> 1920 gives 1920,
    // not 1920.0001), and a hair off a whole pixel is snapped onto it.
    auto scale = [](float value, int toSize, int fromSize) {
        const double scaled = static_cast<double>(value) * toSize / fromSize;
        const double whole = std::round(scaled);
        return static_cast<float>(std::abs(scaled - whole) < 1e-3 ? whole : scaled);
    };
    for (Slice& slice : slices) {
        CanvasRect& in = slice.input;
        const float left = scale(in.x, to.x, from.x);
        const float top = scale(in.y, to.y, from.y);
        const float right = scale(in.x + in.width, to.x, from.x);
        const float bottom = scale(in.y + in.height, to.y, from.y);
        in = CanvasRect{left, top, right - left, bottom - top};
        slice.blend[SLICE_EDGE_LEFT].width = scale(slice.blend[SLICE_EDGE_LEFT].width, to.x, from.x);
        slice.blend[SLICE_EDGE_RIGHT].width = scale(slice.blend[SLICE_EDGE_RIGHT].width, to.x, from.x);
        slice.blend[SLICE_EDGE_TOP].width = scale(slice.blend[SLICE_EDGE_TOP].width, to.y, from.y);
        slice.blend[SLICE_EDGE_BOTTOM].width = scale(slice.blend[SLICE_EDGE_BOTTOM].width, to.y, from.y);
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

std::vector<AutoBlendEdge> autoBlendEdges(const std::vector<SlicePlacement>& slices) {
    std::vector<AutoBlendEdge> edges;
    auto widen = [&](size_t slice, SliceEdge edge, float width) {
        for (AutoBlendEdge& e : edges) {
            if (e.slice == slice && e.edge == edge) {
                e.width = std::max(e.width, width);
                return;
            }
        }
        edges.push_back(AutoBlendEdge{slice, edge, width});
    };
    for (size_t i = 0; i < slices.size(); ++i) {
        for (size_t j = i + 1; j < slices.size(); ++j) {
            if (slices[i].output == slices[j].output) continue;
            const CanvasRect& a = slices[i].input;
            const CanvasRect& b = slices[j].input;
            const float overlapW = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
            const float overlapH = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
            if (overlapW <= 0.0f || overlapH <= 0.0f) continue;
            // One lies further along the axis than the other: neither spans the other.
            const bool offsetX = (a.x < b.x && a.x + a.width < b.x + b.width) ||
                                 (b.x < a.x && b.x + b.width < a.x + a.width);
            const bool offsetY = (a.y < b.y && a.y + a.height < b.y + b.height) ||
                                 (b.y < a.y && b.y + b.height < a.y + a.height);
            const float shareX = overlapW / std::min(a.width, b.width);
            const float shareY = overlapH / std::min(a.height, b.height);
            bool sideBySide = offsetX && shareY >= 0.5f;
            bool stacked = offsetY && shareX >= 0.5f;
            if (sideBySide && stacked) {
                sideBySide = shareX <= shareY;
                stacked = !sideBySide;
            }
            if (sideBySide) {
                const bool aLeft = a.x < b.x;
                widen(aLeft ? i : j, SLICE_EDGE_RIGHT, overlapW);
                widen(aLeft ? j : i, SLICE_EDGE_LEFT, overlapW);
            } else if (stacked) {
                const bool aTop = a.y < b.y;
                widen(aTop ? i : j, SLICE_EDGE_BOTTOM, overlapH);
                widen(aTop ? j : i, SLICE_EDGE_TOP, overlapH);
            }
        }
    }
    return edges;
}

} // namespace quadmapper
