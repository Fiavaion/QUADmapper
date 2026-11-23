#include "MeshSubdivision.hpp"
#include <algorithm>

namespace quadmapper {

namespace {

// Two triangles per cell of a columns x rows grid of (columns + 1)-wide vertex rows.
void gridIndices(int columns, int rows, std::vector<uint32_t>& indices) {
    indices.clear();
    indices.reserve(static_cast<size_t>(columns) * rows * 6);
    const int line = columns + 1;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < columns; ++col) {
            const uint32_t tl = static_cast<uint32_t>(row * line + col);
            const uint32_t tr = tl + 1;
            const uint32_t bl = static_cast<uint32_t>((row + 1) * line + col);
            const uint32_t br = bl + 1;
            indices.push_back(tl); indices.push_back(bl); indices.push_back(tr);
            indices.push_back(tr); indices.push_back(bl); indices.push_back(br);
        }
    }
}

}  // namespace

IVec2 MeshSubdivision::generateMesh(const QuadGeometry& geometry,
                                    std::vector<MeshVertex>& vertices,
                                    std::vector<uint32_t>& indices) {
    const std::vector<float> us = geometry.tessellationU();
    const std::vector<float> vs = geometry.tessellationV();
    const std::vector<QuadGeometry::SurfaceSample> samples = geometry.sampleSurface(us, vs);
    const std::vector<Vec3> image = geometry.imageCoords(us, vs, samples);

    vertices.resize(samples.size());
    for (size_t r = 0; r < vs.size(); ++r) {
        for (size_t c = 0; c < us.size(); ++c) {
            const size_t index = r * us.size() + c;
            const QuadGeometry::SurfaceSample& s = samples[index];
            vertices[index].position = s.position;
            vertices[index].texCoord = Vec3(us[c] * s.q, vs[r] * s.q, s.q);
            vertices[index].imageCoord = image[index];
        }
    }
    const IVec2 cells(static_cast<int>(us.size()) - 1, static_cast<int>(vs.size()) - 1);
    gridIndices(cells.x, cells.y, indices);
    return cells;
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
    gridIndices(resolution, resolution, indices);
}

} // namespace quadmapper
