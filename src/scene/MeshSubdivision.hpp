#pragma once

#include "core/Types.hpp"
#include "QuadGeometry.hpp"
#include <vector>

namespace quadmapper {

// Vertex for rendering
struct MeshVertex {
    Vec2 position;
    Vec2 texCoord;
};

// Mesh generation utilities
class MeshSubdivision {
public:
    // Generate mesh vertices and indices for a quad
    static void generateMesh(const QuadGeometry& geometry,
                            std::vector<MeshVertex>& vertices,
                            std::vector<uint32_t>& indices);

    // Generate just the positions (for preview)
    static std::vector<Vec2> generatePositions(const QuadGeometry& geometry);

    // Calculate vertex index for row/col
    static int getVertexIndex(int row, int col, int resolution);

    // Calculate the number of vertices for a given resolution
    static int getVertexCount(int resolution);

    // Calculate the number of indices for a given resolution
    static int getIndexCount(int resolution);

    // Calculate the number of triangles for a given resolution
    static int getTriangleCount(int resolution);

    // Validate resolution is within allowed range
    static int clampResolution(int resolution);

    // Check if a resolution is valid
    static bool isValidResolution(int resolution);

    // Generate indices only (useful when geometry changes but resolution doesn't)
    static void generateIndices(int resolution, std::vector<uint32_t>& indices);

    // Generate bilinear positions without applying mesh offsets
    static std::vector<Vec2> generateBilinearPositions(const QuadGeometry& geometry);
};

} // namespace quadmapper
