#pragma once

#include "core/Types.hpp"
#include "core/UUID.hpp"
#include "QuadGeometry.hpp"
#include <array>
#include <string>
#include <vector>

namespace quadmapper {

// A rectangle on a scene's canvas, in canvas pixels (origin top-left, y down).
struct CanvasRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 1920.0f;
    float height = 1080.0f;

    bool operator==(const CanvasRect& o) const {
        return x == o.x && y == o.y && width == o.width && height == o.height;
    }
    bool operator!=(const CanvasRect& o) const { return !(*this == o); }
};

// Edges of a slice, in the order Slice::blend stores them.
enum SliceEdge : int { SLICE_EDGE_LEFT = 0, SLICE_EDGE_RIGHT = 1, SLICE_EDGE_TOP = 2, SLICE_EDGE_BOTTOM = 3 };
constexpr int SLICE_EDGE_COUNT = 4;

// One edge's blend zone (ADR-047 §2). Across the zone the slice's light fades from
// full at its inner side to none at the edge, so two projectors whose zones cover the
// same overlap add up to one flat image: the ramp is in linear light, f(t) + f(1 - t)
// = 1, and the pixel gain is f^(1/gamma) so the projector's gamma turns it back into
// f. Width 0 turns the edge off.
struct BlendEdge {
    float width = 0.0f;  // canvas pixels, measured inward from the slice edge
    float gamma = 2.2f;  // the projector's gamma
    float curve = 2.0f;  // ramp exponent: 1 = linear crossfade, larger = flatter ends

    bool operator==(const BlendEdge& o) const {
        return width == o.width && gamma == o.gamma && curve == o.curve;
    }
    bool operator!=(const BlendEdge& o) const { return !(*this == o); }
};

// Load-time and edit bounds for blend parameters.
constexpr float MIN_BLEND_GAMMA = 0.1f;
constexpr float MAX_BLEND_GAMMA = 10.0f;
constexpr float MIN_BLEND_CURVE = 0.1f;
constexpr float MAX_BLEND_CURVE = 10.0f;
constexpr float MAX_BLACK_LEVEL = 0.5f;

// The four corners of the whole output frame (TL, TR, BR, BL): an unwarped slice.
QuadGeometry fullFrameWarp();

// What an output shows of its scene's canvas (ADR-047 §2): the input rectangle, drawn
// through the warp into the output's normalized [0, 1] space, faded at its edges by
// the blend zones. `warp` is an ordinary QuadGeometry, so a grid warp goes through the
// same evaluator and tessellator as a quad (ADR-049).
struct Slice {
    UUID id;
    std::string name = "Slice";
    CanvasRect input;
    QuadGeometry warp = fullFrameWarp();
    std::array<BlendEdge, SLICE_EDGE_COUNT> blend{};
    // Lift of black outside the blend zones, 0-MAX_BLACK_LEVEL of full scale, so the
    // single-projector area can match the brighter black where two projectors overlap.
    float blackLevel = 0.0f;
};

// The slice every output gets by default, and every output of a pre-3.4 file: the
// whole canvas, unwarped, no blend. Renders exactly as the output did before slices.
Slice makeFullCanvasSlice(IVec2 canvasSize);

// True when the warp maps the unit square onto the whole output unchanged.
bool isIdentityWarp(const QuadGeometry& warp);

// True for the slicing every output starts with: one slice of the whole `canvas`,
// unwarped, with no blend zone and no black level.
bool isDefaultSlicing(const std::vector<Slice>& slices, IVec2 canvas);

// Scale every slice's input rectangle and blend widths from one canvas size to
// another, keeping them at the same place on the picture (a scene change or a canvas
// resize). Rectangle edges that land within 1/1000 pixel of a whole pixel are put on
// it, so a slice of the whole old canvas becomes exactly the whole new canvas (and
// keeps the exact-copy path). A non-positive size on either side leaves the slices
// unchanged.
void rescaleSlices(std::vector<Slice>& slices, IVec2 from, IVec2 to);

// Linear-light blend ramp at t in [0, 1] (0 = the slice edge, 1 = the inner side of
// the zone): 0.5 (2t)^p below one half, 1 - 0.5 (2 (1 - t))^p above. For any curve p,
// blendRamp(t) + blendRamp(1 - t) == 1. t outside [0, 1] clamps.
float blendRamp(float t, float curve);

// The pixel gain of one edge at `distance` canvas pixels inward from that edge:
// blendRamp^(1/gamma), 1 outside the zone or when the edge is off.
float blendEdgeGain(const BlendEdge& edge, float distance);

// The slice's pixel gain at a point `inSlice` canvas pixels from the input
// rectangle's top-left corner: the product of the four edges' gains. The slice
// shader computes the same (tests compare against this).
float sliceBlendGain(const Slice& slice, Vec2 inSlice);

// True when a point `inSlice` pixels into the slice lies in no blend zone, where the
// black level applies.
bool outsideBlendZones(const Slice& slice, Vec2 inSlice);

} // namespace quadmapper
