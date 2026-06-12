#pragma once

#include "core/Types.hpp"
#include <vector>
#include <array>

namespace quadmapper {

// Constants
constexpr int DEFAULT_MESH_RESOLUTION = 16;
constexpr int MIN_MESH_RESOLUTION = 2;
constexpr int MAX_MESH_RESOLUTION = 64;

// Quad geometry - corners and mesh
class QuadGeometry {
public:
    QuadGeometry();

    // Corner access (TL, TR, BR, BL)
    const std::array<Vec2, 4>& getCorners() const { return corners_; }
    void setCorner(int index, const Vec2& pos);
    void setCorners(const std::array<Vec2, 4>& corners);
    void resetCorners();  // Reset to unit quad

    // Mesh resolution
    int getMeshResolution() const { return meshResolution_; }
    void setMeshResolution(int resolution);

    // Mesh mode
    MeshMode getMeshMode() const { return meshMode_; }
    void setMeshMode(MeshMode mode) { meshMode_ = mode; }

    // Mesh vertices (only used in MESH_EDIT mode)
    const std::vector<Vec2>& getMeshVertices() const { return meshVertices_; }
    void setMeshVertices(const std::vector<Vec2>& vertices);
    void setMeshVertex(int row, int col, const Vec2& offset);
    void setMeshVertexByIndex(int index, const Vec2& offset);
    Vec2 getMeshVertex(int row, int col) const;
    Vec2 getMeshVertexByIndex(int index) const;
    void resetMesh();  // Reset mesh offsets to zero

    // Check if mesh has any non-zero offsets
    bool hasMeshModifications() const;

    // Get the expected vertex count for current resolution
    int getVertexCount() const;

    // Calculate interpolated position for UV coordinate
    Vec2 transformUVToPosition(const Vec2& uv) const;

    // Generate mesh vertex positions (combines corners + mesh offsets)
    std::vector<Vec2> generateMeshPositions() const;

    // Generate bilinear positions only (ignores mesh offsets)
    std::vector<Vec2> generateBilinearPositions() const;

private:
    std::array<Vec2, 4> corners_;  // TL, TR, BR, BL in normalized [0,1]
    int meshResolution_;
    MeshMode meshMode_;
    std::vector<Vec2> meshVertices_;  // Offsets from bilinear positions

    void initializeMeshVertices();
};

} // namespace quadmapper
