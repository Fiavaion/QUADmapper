#pragma once

#include "core/Types.hpp"
#include <vector>
#include <array>

namespace quadmapper {

// Constants
// A new quad starts as a single cell (1x1) — no internal divisions — so the user
// adds division lines deliberately (ADR-038 stopgap). Render density does not depend
// on the control grid (ADR-049 §4, see tessellationU()).
constexpr int DEFAULT_MESH_RESOLUTION = 1;
constexpr int MIN_MESH_RESOLUTION = 1;
constexpr int MAX_MESH_RESOLUTION = 64;

// Render tessellation (ADR-049 §4): at least this many segments per axis, and at
// least RENDER_MIN_SEGMENTS_PER_CELL per control cell, so a 64-cell axis gets 128.
constexpr int RENDER_MIN_SEGMENTS = 32;
constexpr int RENDER_MIN_SEGMENTS_PER_CELL = 2;

// Tangent handles of a control vertex along its grid lines (ADR-049 §3, QM-079):
// one on each side of the vertex along its row (U) and along its column (V).
enum GridHandle : int {
    GRID_HANDLE_U_BEFORE = 0,  // towards the previous column
    GRID_HANDLE_U_AFTER = 1,   // towards the next column
    GRID_HANDLE_V_BEFORE = 2,  // towards the previous row
    GRID_HANDLE_V_AFTER = 3,   // towards the next row
};
constexpr int GRID_HANDLE_COUNT = 4;

// How the corner-pin base B maps the unit square onto the corners (ADR-058).
// PERSPECTIVE: the homography, perspective-correct inside the quad (ADR-043).
// LINEAR: the bilinear patch, evenly spaced along every edge, so two quads that share
// an edge always line up along it.
enum class WarpMode : int {
    PERSPECTIVE = 0,
    LINEAR = 1,
};

// What a quad's mesh warp does to its image (owner, 2026-11-23).
// WARP: the image follows the surface, so bending the mesh bends the picture.
// CROP: the image stays on the plain corner pin (no curved edges, no mesh) and the
// bent outline cuts a shape out of it; outside the image is transparent.
enum class ContentMode : int {
    WARP = 0,
    CROP = 1,
};

// Explicit tangents of one control vertex, indexed by GridHandle: the derivative of
// the residual along u (U handles) or v (V handles) on that side. A handle whose bit
// is clear in explicitMask uses the automatic tangent instead: Catmull-Rom for a
// Spline point, the one-sided chord to the neighbour for a Linear one (owner,
// 2026-11-23), so the grid runs straight between Linear points.
struct VertexTangents {
    std::array<Vec2, GRID_HANDLE_COUNT> derivative{Vec2(0.0f), Vec2(0.0f), Vec2(0.0f), Vec2(0.0f)};
    unsigned explicitMask = 0;
    bool linear = false;  // Linear point: straight grid lines to its neighbours

    bool isExplicit(int handle) const { return (explicitMask >> handle) & 1u; }
    bool operator==(const VertexTangents& o) const {
        return explicitMask == o.explicitMask && linear == o.linear && derivative == o.derivative;
    }
    bool operator!=(const VertexTangents& o) const { return !(*this == o); }
};

// Quad geometry — corners + a control grid (ADR-038) on one surface (ADR-049).
//
// The mesh is a *control grid* defined by independent division positions along U
// (uCoords_) and V (vCoords_), each a sorted list in [0,1] with endpoints 0 and 1.
// A uniform N×N grid is just evenly-spaced divisions (setUniformGrid / the legacy
// setMeshResolution). Per-control-vertex residuals (meshVertices_) and tangents are
// stored in row-major order, vertsPerRow() entries per row.
//
// The surface is S(u,v) = B(u,v) + R(u,v) (ADR-049). B is the corner-pin base
// (basePoint). R is the residual: each control vertex stores its offset from B at its
// grid parameters, and a bicubic Hermite patch per cell interpolates between them
// (Catmull-Rom tangents unless a handle set one). R applies in MESH_EDIT only, and
// with all residuals zero S equals B exactly, so toggling mesh edit moves nothing.
class QuadGeometry {
public:
    QuadGeometry();

    // Corner access (TL, TR, BR, BL)
    const std::array<Vec2, 4>& getCorners() const { return corners_; }
    void setCorner(int index, const Vec2& pos);
    void setCorners(const std::array<Vec2, 4>& corners);
    void resetCorners();  // Reset to unit quad

    // --- Control grid -------------------------------------------------------
    // Division coordinates along each axis (sorted, include 0 and 1).
    const std::vector<float>& getUCoords() const { return uCoords_; }
    const std::vector<float>& getVCoords() const { return vCoords_; }
    float getUCoord(int col) const;
    float getVCoord(int row) const;

    // Grid dimensions. columns/rows = number of cells; vertsPerRow/Col = number
    // of grid lines along that axis (= cells + 1).
    int getColumns() const { return static_cast<int>(uCoords_.size()) - 1; }
    int getRows() const { return static_cast<int>(vCoords_.size()) - 1; }
    int vertsPerRow() const { return static_cast<int>(uCoords_.size()); }
    int vertsPerCol() const { return static_cast<int>(vCoords_.size()); }

    // Reset the grid to an even n×n division (the "even grid" convenience action,
    // ADR-038). Clears all mesh offsets. n is clamped to [MIN,MAX]_MESH_RESOLUTION.
    void setUniformGrid(int n);

    // Replace the division coordinates wholesale (used by deserialization). Both
    // lists must have >= 2 entries; endpoints are forced to exactly 0 and 1.
    // Resizes (and zero-fills) the offset store to match; caller sets offsets after.
    void setGridCoords(const std::vector<float>& u, const std::vector<float>& v);

    // Insert a division line at normalized position p (0<p<1). No-op if it would
    // duplicate an existing line. New control vertices take the residual the
    // surface already has there, so they land on it, and are Linear or Spline points
    // as `linear` says. Returns the index of the inserted line (its column/row), or
    // -1 if rejected (out of range / dup).
    int insertUDivision(float u, bool linear = false);
    int insertVDivision(float v, bool linear = false);
    // Move an existing interior division line to a new normalized position, clamped
    // strictly between its neighbours (endpoints are pinned and cannot move). Used
    // for drag-to-position placement. No-op for endpoint indices.
    void moveUDivision(int col, float u);
    void moveVDivision(int row, float v);
    // Remove an interior division line by index (1..count-1; endpoints can't be
    // removed). Preserves the residuals and tangents of the remaining lines.
    void removeUDivision(int col);
    void removeVDivision(int row);

    // Legacy uniform-resolution accessors (kept for the UI/commands/serializer).
    // getMeshResolution() returns the column count; setMeshResolution() builds an
    // even grid (== setUniformGrid). Meaningful only for uniform grids.
    int getMeshResolution() const { return getColumns(); }
    void setMeshResolution(int resolution) { setUniformGrid(resolution); }
    bool isUniform() const;  // true when both axes are evenly spaced and equal

    // Mesh mode
    MeshMode getMeshMode() const { return meshMode_; }
    void setMeshMode(MeshMode mode) { meshMode_ = mode; }

    // Warp mode (ADR-058). Default PERSPECTIVE; QuadManager::createQuad makes LINEAR
    // quads. Switching keeps every control vertex where it is: residuals are
    // re-expressed against the new base.
    WarpMode getWarpMode() const { return warpMode_; }
    void setWarpMode(WarpMode mode);

    // Warp or Crop (ContentMode). Default WARP.
    ContentMode getContentMode() const { return contentMode_; }
    void setContentMode(ContentMode mode) { contentMode_ = mode; }
    // True when the image is not drawn on the surface itself: CROP, and the surface
    // differs from the plain corner pin (a mesh residual or curved edges).
    bool cropsImage() const;

    // --- Bezier edges (ADR-043) --------------------------------------------
    // Optional cubic-bezier edges on the 4 corners. Each of the 4 edges has 2 control
    // points, stored as OFFSETS from the straight-line 1/3 and 2/3 points — so zero
    // offsets == straight edges and the controls track corner moves. When enabled,
    // basePoint() blends the four edge curves into a Coons patch; the renderer
    // tessellates that finely, so edges can bow smoothly.
    bool isBezierEnabled() const { return bezierEnabled_; }
    void setBezierEnabled(bool on) { bezierEnabled_ = on; }
    // edge: 0=TL->TR (top), 1=TR->BR (right), 2=BR->BL (bottom), 3=BL->TL (left).
    // which: 0 = the 1/3 control point, 1 = the 2/3 control point.
    Vec2 getBezierControl(int edge, int which) const;          // effective position
    void setBezierControl(int edge, int which, const Vec2& pos); // stores offset
    const std::array<Vec2, 8>& getBezierOffsets() const { return bezierOffsets_; }
    void setBezierOffsets(const std::array<Vec2, 8>& o) { bezierOffsets_ = o; }
    void resetBezier();              // zero all offsets (straight) and disable
    bool hasBezierEdges() const;     // enabled AND at least one non-zero offset

    // Control-vertex residuals: each vertex's offset from the base B at its grid
    // parameters (ADR-049). Applied in MESH_EDIT mode only.
    const std::vector<Vec2>& getMeshVertices() const { return meshVertices_; }
    void setMeshVertices(const std::vector<Vec2>& vertices);
    void setMeshVertex(int row, int col, const Vec2& offset);
    void setMeshVertexByIndex(int index, const Vec2& offset);
    Vec2 getMeshVertex(int row, int col) const;
    Vec2 getMeshVertexByIndex(int index) const;
    void resetMesh();  // zero every residual and drop every explicit tangent; keeps Linear

    // Linear / Spline control points (owner, 2026-11-23). Making a point Linear drops
    // its explicit handles; dragging a handle makes the point Spline again.
    bool isVertexLinear(int index) const;
    void setVertexLinear(int index, bool linear);
    void setAllVerticesLinear(bool linear);
    bool hasLinearVertices() const;

    // True when any residual is non-zero or any tangent handle is explicit.
    bool hasMeshModifications() const;

    // Total control-grid vertex count (vertsPerRow * vertsPerCol)
    int getVertexCount() const;

    // Files written before ADR-049 stored offsets from the bilinear corner patch.
    // Re-express them as residuals against B so every control vertex keeps its
    // absolute position. Call once, after the corners, grid, offsets and bezier
    // edges are all loaded.
    void convertBilinearOffsetsToResiduals();

    // --- Grid tangent handles (ADR-049 §3, QM-079) ---------------------------
    const std::vector<VertexTangents>& getVertexTangents() const { return tangents_; }
    void setVertexTangents(const std::vector<VertexTangents>& tangents);  // size must match
    VertexTangents getVertexTangent(int index) const;
    void setVertexTangent(int index, const VertexTangents& tangents);
    bool hasExplicitTangents() const;
    // Bit h set when handle h exists on this vertex: a boundary vertex has no handle
    // pointing out of the grid.
    unsigned availableHandles(int index) const;
    // Canvas position of a handle, in the bezier convention: the vertex position plus
    // a third of the adjacent cell's width times the surface derivative on that side.
    // Uses the residuals whatever the mesh mode (handles are a mesh-edit tool).
    Vec2 gridHandlePosition(int index, int handle) const;
    // Move a handle to `pos`, making its tangent explicit. No-op for a handle the
    // vertex does not have.
    void setGridHandlePosition(int index, int handle, const Vec2& pos);

    // Bilinear corner interpolation. Not the surface: files written before ADR-049
    // stored their mesh offsets against it (convertBilinearOffsetsToResiduals).
    Vec2 transformUVToPosition(const Vec2& uv) const;

    // The base B at parametric (u,v) in [0,1] (ADR-043): in PERSPECTIVE the projective
    // map (homography) of the unit square onto the corners, so a corner-pinned flat
    // surface is perspective-correct; LINEAR, concave or degenerate corners use the
    // bilinear patch (ADR-058). With bezier edges enabled, the edge curves' deviations
    // from their chords are blended over it as a Coons patch.
    Vec2 basePoint(float u, float v) const;

    // The surface S = B + R at (u,v); R only in MESH_EDIT.
    Vec2 surfacePoint(float u, float v) const;

    // One tessellation vertex: its position on S, and q = 1/w of the projective base
    // (1 for the bilinear fallback). Interpolating (u*q, v*q, q) linearly across a
    // triangle and dividing by q per fragment reproduces the homography exactly inside
    // it; a residual moves the vertex without changing its q.
    struct SurfaceSample {
        Vec2 position;
        float q;
    };

    // Row-major samples of S at every (us[c], vs[r]); both lists sorted, in [0,1].
    std::vector<SurfaceSample> sampleSurface(const std::vector<float>& us,
                                             const std::vector<float>& vs) const;

    // True when S differs from B: MESH_EDIT with a residual or an explicit tangent.
    bool hasResidualSurface() const;

    // Render tessellation parameters (ADR-049 §4). While S == B: RENDER_MIN_SEGMENTS
    // even segments. Otherwise each control cell is split evenly into
    // max(RENDER_MIN_SEGMENTS_PER_CELL, ceil(RENDER_MIN_SEGMENTS / cells)) segments, so
    // every control line is a tessellation line (exact at the control vertices) and
    // the axis has at least RENDER_MIN_SEGMENTS segments (at most 128).
    std::vector<float> tessellationU() const;
    std::vector<float> tessellationV() const;

    // The (u,v) the renderer draws at canvas point p: the tessellation triangle that
    // contains p, interpolated projectively as the fragment shader does. (-1,-1) when
    // p is outside the surface. Hit-testing and click-to-insert use this.
    Vec2 surfaceParamsAt(const Vec2& p) const;

    // The (u,v) of the point of the drawn outline nearest p, when that point is within
    // maxDistance of p; else (-1,-1). Click-add uses it for a press on the edge.
    Vec2 boundaryParamsNear(const Vec2& p, float maxDistance) const;

    // The interior grid line drawn nearest p within maxDistance: its column index
    // (isColumn, a U line) or row index (a V line), else -1. Lines are drawn on S, so
    // this follows the warp.
    int gridLineNear(const Vec2& p, float maxDistance, bool& isColumn) const;

    // Image coordinates for samples of S taken at (us, vs) by sampleSurface: where
    // each lands in the image, as (s*q, t*q, q) for quad.frag. Without cropsImage()
    // that is the sample's own (u*q, v*q, q). With it, the point of the plain corner
    // pin under the sample (outside [0,1] when the outline leaves the image).
    std::vector<Vec3> imageCoords(const std::vector<float>& us, const std::vector<float>& vs,
                                  const std::vector<SurfaceSample>& samples) const;

    // Control-vertex positions on S (residuals only in MESH_EDIT).
    std::vector<Vec2> generateMeshPositions() const;

    // B at every control vertex: where the vertices sit with zero residuals.
    std::vector<Vec2> generateBasePositions() const;

private:
    std::array<Vec2, 4> corners_;  // TL, TR, BR, BL in normalized [0,1]
    std::vector<float> uCoords_;   // U division lines, sorted, [0..1] inclusive
    std::vector<float> vCoords_;   // V division lines, sorted, [0..1] inclusive
    MeshMode meshMode_;
    WarpMode warpMode_ = WarpMode::PERSPECTIVE;
    ContentMode contentMode_ = ContentMode::WARP;
    std::vector<Vec2> meshVertices_;        // residuals from B (row-major)
    std::vector<VertexTangents> tangents_;  // per control vertex (row-major)

    bool bezierEnabled_ = false;
    std::array<Vec2, 8> bezierOffsets_;  // 2 per edge; offset from 1/3 & 2/3 points

    // The 8 effective bezier control points (2 per edge, edges in corner order).
    std::array<Vec2, 8> bezierControls() const;

    // B + R at a control vertex, whatever the mesh mode.
    Vec2 meshedVertex(int index) const;
    // dB/du (alongU) or dB/dv at a control vertex. B is smooth, so both sides agree.
    Vec2 baseDerivative(int index, bool alongU) const;
    // The adjacent cell's parameter width on a handle's side, and the side's sign
    // (-1 before, +1 after).
    float handleSpan(int index, int handle, float& sign) const;

    static std::vector<float> evenCoords(int n);
    static std::vector<float> tessellationParams(const std::vector<float>& grid,
                                                 bool residual);
    void initializeMeshVertices();
    int vertexIndex(int row, int col) const { return row * vertsPerRow() + col; }
};

} // namespace quadmapper
