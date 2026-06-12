#include "QuadGeometry.hpp"
#include <algorithm>
#include <cmath>

namespace quadmapper {

QuadGeometry::QuadGeometry()
    : meshResolution_(DEFAULT_MESH_RESOLUTION)
    , meshMode_(MeshMode::CORNERS_ONLY) {
    resetCorners();
    initializeMeshVertices();
}

void QuadGeometry::setCorner(int index, const Vec2& pos) {
    if (index >= 0 && index < 4) {
        corners_[index] = pos;
    }
}

void QuadGeometry::setCorners(const std::array<Vec2, 4>& corners) {
    corners_ = corners;
}

void QuadGeometry::resetCorners() {
    // Default quad: 40% of canvas, centered
    // 40% width/height centered at 0.5 means 0.3 to 0.7
    corners_[0] = Vec2(0.3f, 0.3f);  // TL
    corners_[1] = Vec2(0.7f, 0.3f);  // TR
    corners_[2] = Vec2(0.7f, 0.7f);  // BR
    corners_[3] = Vec2(0.3f, 0.7f);  // BL
}

void QuadGeometry::setMeshResolution(int resolution) {
    resolution = std::clamp(resolution, MIN_MESH_RESOLUTION, MAX_MESH_RESOLUTION);
    if (resolution != meshResolution_) {
        meshResolution_ = resolution;
        initializeMeshVertices();
    }
}

void QuadGeometry::setMeshVertices(const std::vector<Vec2>& vertices) {
    int expectedCount = getVertexCount();
    if (static_cast<int>(vertices.size()) == expectedCount) {
        meshVertices_ = vertices;
    }
}

void QuadGeometry::setMeshVertex(int row, int col, const Vec2& offset) {
    int index = row * (meshResolution_ + 1) + col;
    setMeshVertexByIndex(index, offset);
}

void QuadGeometry::setMeshVertexByIndex(int index, const Vec2& offset) {
    if (index >= 0 && index < static_cast<int>(meshVertices_.size())) {
        meshVertices_[index] = offset;
    }
}

Vec2 QuadGeometry::getMeshVertex(int row, int col) const {
    int index = row * (meshResolution_ + 1) + col;
    return getMeshVertexByIndex(index);
}

Vec2 QuadGeometry::getMeshVertexByIndex(int index) const {
    if (index >= 0 && index < static_cast<int>(meshVertices_.size())) {
        return meshVertices_[index];
    }
    return Vec2(0.0f);
}

void QuadGeometry::resetMesh() {
    std::fill(meshVertices_.begin(), meshVertices_.end(), Vec2(0.0f));
}

bool QuadGeometry::hasMeshModifications() const {
    constexpr float EPSILON = 1e-6f;
    for (const auto& offset : meshVertices_) {
        if (std::abs(offset.x) > EPSILON || std::abs(offset.y) > EPSILON) {
            return true;
        }
    }
    return false;
}

int QuadGeometry::getVertexCount() const {
    return (meshResolution_ + 1) * (meshResolution_ + 1);
}

void QuadGeometry::initializeMeshVertices() {
    int count = getVertexCount();
    meshVertices_.clear();
    meshVertices_.resize(count, Vec2(0.0f));
}

Vec2 QuadGeometry::transformUVToPosition(const Vec2& uv) const {
    // Bilinear interpolation
    Vec2 top = glm::mix(corners_[0], corners_[1], uv.x);
    Vec2 bottom = glm::mix(corners_[3], corners_[2], uv.x);
    return glm::mix(top, bottom, uv.y);
}

std::vector<Vec2> QuadGeometry::generateMeshPositions() const {
    int vertexCount = getVertexCount();
    std::vector<Vec2> positions(vertexCount);

    for (int row = 0; row <= meshResolution_; ++row) {
        for (int col = 0; col <= meshResolution_; ++col) {
            float u = static_cast<float>(col) / meshResolution_;
            float v = static_cast<float>(row) / meshResolution_;

            Vec2 pos = transformUVToPosition(Vec2(u, v));

            // Add mesh offset if in edit mode
            if (meshMode_ == MeshMode::MESH_EDIT) {
                int index = row * (meshResolution_ + 1) + col;
                pos += meshVertices_[index];
            }

            int index = row * (meshResolution_ + 1) + col;
            positions[index] = pos;
        }
    }

    return positions;
}

std::vector<Vec2> QuadGeometry::generateBilinearPositions() const {
    int vertexCount = getVertexCount();
    std::vector<Vec2> positions(vertexCount);

    for (int row = 0; row <= meshResolution_; ++row) {
        for (int col = 0; col <= meshResolution_; ++col) {
            float u = static_cast<float>(col) / meshResolution_;
            float v = static_cast<float>(row) / meshResolution_;

            Vec2 pos = transformUVToPosition(Vec2(u, v));

            int index = row * (meshResolution_ + 1) + col;
            positions[index] = pos;
        }
    }

    return positions;
}

} // namespace quadmapper
