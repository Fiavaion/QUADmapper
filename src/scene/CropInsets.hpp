#pragma once

#include "core/Types.hpp"

#include <algorithm>
#include <cmath>

namespace quadmapper {

// Edge-inset view of a crop rectangle (ADR-040). The crop is stored as a normalized
// {x, y, w, h}; the inspector edits it as four insets (left = x, right = 1 - (x + w),
// top = y, bottom = 1 - (y + h)), either as fractions or as source pixels. These are
// pure conversions so the UI never re-derives the maths.
enum class CropEdge { Left = 0, Top = 1, Right = 2, Bottom = 3 };

namespace crop_detail {

inline double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

inline CropEdge opposite(CropEdge edge) {
    switch (edge) {
        case CropEdge::Left:  return CropEdge::Right;
        case CropEdge::Right: return CropEdge::Left;
        case CropEdge::Top:   return CropEdge::Bottom;
        default:              return CropEdge::Top;
    }
}

inline bool horizontal(CropEdge edge) {
    return edge == CropEdge::Left || edge == CropEdge::Right;
}

} // namespace crop_detail

// Normalized inset of one edge, clamped to [0, 1].
inline double cropInset(const NormalizedRect& crop, CropEdge edge) {
    const double x = crop.x, y = crop.y, w = crop.width, h = crop.height;
    switch (edge) {
        case CropEdge::Left:  return crop_detail::clamp01(x);
        case CropEdge::Right: return crop_detail::clamp01(1.0 - (x + w));
        case CropEdge::Top:   return crop_detail::clamp01(y);
        default:              return crop_detail::clamp01(1.0 - (y + h));
    }
}

// Replaces one edge's normalized inset. The inset is clamped so at least minRegion is
// left between it and the opposite edge (which keeps its stored value), so the region
// never inverts. The other axis is carried over unchanged (re-clamped only).
inline NormalizedRect withCropInset(const NormalizedRect& crop, CropEdge edge,
                                    double inset, double minRegion) {
    double left   = cropInset(crop, CropEdge::Left);
    double right  = cropInset(crop, CropEdge::Right);
    double top    = cropInset(crop, CropEdge::Top);
    double bottom = cropInset(crop, CropEdge::Bottom);

    const double maxInset =
        std::max(0.0, 1.0 - cropInset(crop, crop_detail::opposite(edge)) - minRegion);
    const double value = std::clamp(inset, 0.0, maxInset);
    switch (edge) {
        case CropEdge::Left:  left = value; break;
        case CropEdge::Right: right = value; break;
        case CropEdge::Top:   top = value; break;
        default:              bottom = value; break;
    }
    return NormalizedRect{static_cast<float>(left), static_cast<float>(top),
                          static_cast<float>(1.0 - left - right),
                          static_cast<float>(1.0 - top - bottom)};
}

// The source dimension an edge is measured along: width for Left/Right, else height.
inline int cropEdgeSourceSize(CropEdge edge, int width, int height) {
    return crop_detail::horizontal(edge) ? width : height;
}

// One edge's inset in whole source pixels (nearest pixel).
inline int cropInsetPixels(const NormalizedRect& crop, CropEdge edge, int width, int height) {
    const int size = cropEdgeSourceSize(edge, width, height);
    if (size <= 0) return 0;
    return static_cast<int>(std::lround(cropInset(crop, edge) * size));
}

// Largest pixel inset an edge may take while leaving at least one source pixel
// between it and the opposite edge.
inline int maxCropInsetPixels(const NormalizedRect& crop, CropEdge edge, int width,
                              int height) {
    const int size = cropEdgeSourceSize(edge, width, height);
    if (size <= 0) return 0;
    const double opposite = cropInset(crop, crop_detail::opposite(edge)) * size;
    // The 0.01 px slack absorbs float storage error on a pixel-aligned opposite edge
    // (about 1e-3 px at 16K), which would otherwise cost the last pixel.
    const int maxPixels = static_cast<int>(std::floor(size - opposite - 1.0 + 1e-2));
    return std::max(0, maxPixels);
}

// Sets one edge to a whole-pixel inset, clamped to [0, maxCropInsetPixels], stored as
// pixels / size. Right and bottom are implicit in the float {x, w} / {y, h}, so every
// rebuild would nudge them by float rounding and repeated edits would drift them; an
// untouched edge already within 0.01 px of a whole pixel is therefore re-snapped to it.
// Edges that were never pixel-aligned (e.g. a half of an odd width) keep their value.
inline NormalizedRect withCropInsetPixels(const NormalizedRect& crop, CropEdge edge,
                                          int pixels, int width, int height) {
    const int size = cropEdgeSourceSize(edge, width, height);
    if (size <= 0 || width <= 0 || height <= 0) return crop;

    constexpr double SNAP_PIXELS = 1e-2;
    double inset[4];
    for (CropEdge e : {CropEdge::Left, CropEdge::Top, CropEdge::Right, CropEdge::Bottom}) {
        const int edgeSize = cropEdgeSourceSize(e, width, height);
        double value = cropInset(crop, e);
        const double px = value * edgeSize;
        if (e != edge && std::abs(px - std::round(px)) < SNAP_PIXELS) {
            value = std::round(px) / edgeSize;
        }
        inset[static_cast<int>(e)] = value;
    }

    const double opposite = inset[static_cast<int>(crop_detail::opposite(edge))] * size;
    const int maxPixels =
        std::max(0, static_cast<int>(std::floor(size - opposite - 1.0 + SNAP_PIXELS)));
    inset[static_cast<int>(edge)] =
        static_cast<double>(std::clamp(pixels, 0, maxPixels)) / size;

    const double left = inset[static_cast<int>(CropEdge::Left)];
    const double top = inset[static_cast<int>(CropEdge::Top)];
    const double right = inset[static_cast<int>(CropEdge::Right)];
    const double bottom = inset[static_cast<int>(CropEdge::Bottom)];
    return NormalizedRect{static_cast<float>(left), static_cast<float>(top),
                          static_cast<float>(1.0 - left - right),
                          static_cast<float>(1.0 - top - bottom)};
}

} // namespace quadmapper
