#pragma once

#include "core/Types.hpp"
#include "QuadGeometry.hpp"
#include <vector>

namespace quadmapper {

// Vertex for rendering
struct MeshVertex {
    Vec2 position;
    Vec3 texCoord;    // (u*q, v*q, q); quad.frag divides by q for projective mapping
    Vec3 imageCoord;  // where the image is sampled, same form; == texCoord unless Crop
};

// Mesh generation utilities
class MeshSubdivision {
public:
    // The quad's render mesh (ADR-049 §4): its surface S sampled at the tessellation
    // parameters (QuadGeometry::tessellationU/V) with projective texture coordinates,
    // two triangles per tessellation cell, (tl, bl, tr) and (tr, bl, br). Returns the
    // tessellation's cell count along U (x) and V (y).
    static IVec2 generateMesh(const QuadGeometry& geometry,
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

    // The triangles of a resolution x resolution grid, in generateMesh's order.
    static void generateIndices(int resolution, std::vector<uint32_t>& indices);
};

} // namespace quadmapper
