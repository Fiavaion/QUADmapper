#include "MeshSubdivision.hpp"
#include <algorithm>

namespace quadmapper {

void MeshSubdivision::generateMesh(const QuadGeometry& geometry,
                                   std::vector<MeshVertex>& vertices,
                                   std::vector<uint32_t>& indices) {
    int res = geometry.getMeshResolution();
    int vertexCount = getVertexCount(res);

    vertices.clear();
    vertices.reserve(vertexCount);

    // Get mesh positions
    auto positions = geometry.generateMeshPositions();

    // Build vertices
    for (int row = 0; row <= res; ++row) {
        for (int col = 0; col <= res; ++col) {
            MeshVertex v;
            v.position = positions[getVertexIndex(row, col, res)];
            v.texCoord = Vec2(
                static_cast<float>(col) / res,
                static_cast<float>(row) / res
            );
            vertices.push_back(v);
        }
    }

    // Generate indices
    generateIndices(res, indices);
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
    int res = geometry.getMeshResolution();
    int vertexCount = getVertexCount(res);
    std::vector<Vec2> positions(vertexCount);

    for (int row = 0; row <= res; ++row) {
        for (int col = 0; col <= res; ++col) {
            float u = static_cast<float>(col) / res;
            float v = static_cast<float>(row) / res;

            // Get pure bilinear position without any mesh offsets
            Vec2 pos = geometry.transformUVToPosition(Vec2(u, v));

            int index = getVertexIndex(row, col, res);
            positions[index] = pos;
        }
    }

    return positions;
}

} // namespace quadmapper
