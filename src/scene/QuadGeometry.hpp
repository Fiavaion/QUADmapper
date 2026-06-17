#pragma once

#include "core/Types.hpp"
#include <vector>
#include <array>

namespace quadmapper {

// Constants
constexpr int DEFAULT_MESH_RESOLUTION = 16;
constexpr int MIN_MESH_RESOLUTION = 2;
constexpr int MAX_MESH_RESOLUTION = 64;

// Quad geometry — corners + a control grid (ADR-038).
//
// The mesh is a *control grid* defined by independent division positions along U
// (uCoords_) and V (vCoords_), each a sorted list in [0,1] with endpoints 0 and 1.
// A uniform N×N grid is just evenly-spaced divisions (setUniformGrid / the legacy
// setMeshResolution). Per-control-vertex offsets (meshVertices_) are stored in
// row-major order, vertsPerRow() entries per row. Tier 1 = non-uniform linear
// grid; Bezier handles and local subdivision come in later tiers.
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
    // duplicate an existing line. New control-vertex offsets are interpolated from
    // the two neighbouring lines so the surface does not jump.
    void insertUDivision(float u);
    void insertVDivision(float v);
    // Remove an interior division line by index (1..count-1; endpoints can't be
    // removed). Preserves the offsets of the remaining lines.
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

    // Mesh vertices (offsets from bilinear positions; only used in MESH_EDIT mode)
    const std::vector<Vec2>& getMeshVertices() const { return meshVertices_; }
    void setMeshVertices(const std::vector<Vec2>& vertices);
    void setMeshVertex(int row, int col, const Vec2& offset);
    void setMeshVertexByIndex(int index, const Vec2& offset);
    Vec2 getMeshVertex(int row, int col) const;
    Vec2 getMeshVertexByIndex(int index) const;
    void resetMesh();  // Reset mesh offsets to zero

    // Check if mesh has any non-zero offsets
    bool hasMeshModifications() const;

    // Total control-grid vertex count (vertsPerRow * vertsPerCol)
    int getVertexCount() const;

    // Calculate interpolated position for UV coordinate
    Vec2 transformUVToPosition(const Vec2& uv) const;

    // Inverse of the corner bilinear map: the (u,v) parameters of point p inside
    // the quad [TL,TR,BR,BL] (u runs TL->TR, v runs TL->BL). Returns (-1,-1) if p
    // is outside / the solve is degenerate. Used by click-on-edge division insert
    // (ADR-038). Static + corner-only so it is pure and unit-testable.
    static Vec2 inverseBilinear(const std::array<Vec2, 4>& corners, const Vec2& p);

    // Generate control-grid vertex positions (corners + mesh offsets)
    std::vector<Vec2> generateMeshPositions() const;

    // Generate bilinear positions only (ignores mesh offsets)
    std::vector<Vec2> generateBilinearPositions() const;

private:
    std::array<Vec2, 4> corners_;  // TL, TR, BR, BL in normalized [0,1]
    std::vector<float> uCoords_;   // U division lines, sorted, [0..1] inclusive
    std::vector<float> vCoords_;   // V division lines, sorted, [0..1] inclusive
    MeshMode meshMode_;
    std::vector<Vec2> meshVertices_;  // Offsets from bilinear positions (row-major)

    static std::vector<float> evenCoords(int n);
    void initializeMeshVertices();
    int vertexIndex(int row, int col) const { return row * vertsPerRow() + col; }
};

} // namespace quadmapper
