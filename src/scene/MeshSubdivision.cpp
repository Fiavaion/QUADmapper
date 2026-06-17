#include "MeshSubdivision.hpp"
#include <algorithm>

namespace quadmapper {

void MeshSubdivision::generateMesh(const QuadGeometry& geometry,
                                   std::vector<MeshVertex>& vertices,
                                   std::vector<uint32_t>& indices) {
    // Control grid may be non-uniform (ADR-038): iterate per-axis and take tex
    // coords from the actual division positions, not col/res.
    const int vpr = geometry.vertsPerRow();   // grid lines along U
    const int vpc = geometry.vertsPerCol();   // grid lines along V
    const auto& uCoords = geometry.getUCoords();
    const auto& vCoords = geometry.getVCoords();
    auto positions = geometry.generateMeshPositions();

    vertices.clear();
    vertices.reserve(static_cast<size_t>(vpr) * vpc);
    for (int row = 0; row < vpc; ++row) {
        for (int col = 0; col < vpr; ++col) {
            MeshVertex v;
            v.position = positions[static_cast<size_t>(row) * vpr + col];
            v.texCoord = Vec2(uCoords[col], vCoords[row]);
            vertices.push_back(v);
        }
    }

    // Two triangles per cell, indexing the vpr-wide vertex grid.
    indices.clear();
    indices.reserve(static_cast<size_t>(geometry.getColumns()) * geometry.getRows() * 6);
    for (int row = 0; row < vpc - 1; ++row) {
        for (int col = 0; col < vpr - 1; ++col) {
            const uint32_t tl = static_cast<uint32_t>(row * vpr + col);
            const uint32_t tr = tl + 1;
            const uint32_t bl = static_cast<uint32_t>((row + 1) * vpr + col);
            const uint32_t br = bl + 1;
            indices.push_back(tl); indices.push_back(bl); indices.push_back(tr);
            indices.push_back(tr); indices.push_back(bl); indices.push_back(br);
        }
    }
}

std::vector<Vec2> MeshSubdivision::generatePositions(const QuadGeometry& geometry) {
    return geometry.generateMeshPositions();
}

int MeshSubdivision::getVertexIndex(int row, int col, int resolution) {
    return row * (resolution + 1) + col;
}

int MeshSubdivision::getVertexCount(int resolution) {
    return (resolution + 1) * (resolution + 1);
}

int MeshSubdivision::getIndexCount(int resolution) {
    return resolution * resolution * 6;
}

int MeshSubdivision::getTriangleCount(int resolution) {
    return resolution * resolution * 2;
}

int MeshSubdivision::clampResolution(int resolution) {
    return std::clamp(resolution, MIN_MESH_RESOLUTION, MAX_MESH_RESOLUTION);
}

bool MeshSubdivision::isValidResolution(int resolution) {
    return resolution >= MIN_MESH_RESOLUTION && resolution <= MAX_MESH_RESOLUTION;
}

void MeshSubdivision::generateIndices(int resolution, std::vector<uint32_t>& indices) {
    indices.clear();
    indices.reserve(getIndexCount(resolution));

    for (int row = 0; row < resolution; ++row) {
        for (int col = 0; col < resolution; ++col) {
            uint32_t tl = getVertexIndex(row, col, resolution);
            uint32_t tr = getVertexIndex(row, col + 1, resolution);
            uint32_t bl = getVertexIndex(row + 1, col, resolution);
            uint32_t br = getVertexIndex(row + 1, col + 1, resolution);

            // First triangle (TL, BL, TR)
            indices.push_back(tl);
            indices.push_back(bl);
            indices.push_back(tr);

            // Second triangle (TR, BL, BR)
            indices.push_back(tr);
            indices.push_back(bl);
            indices.push_back(br);
        }
    }
}

std::vector<Vec2> MeshSubdivision::generateBilinearPositions(const QuadGeometry& geometry) {
    // Delegates to the geometry, which honours a non-uniform control grid (ADR-038).
    return geometry.generateBilinearPositions();
}

} // namespace quadmapper
