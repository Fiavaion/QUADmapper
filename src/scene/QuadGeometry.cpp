#include "QuadGeometry.hpp"
#include <algorithm>
#include <cmath>

namespace quadmapper {

QuadGeometry::QuadGeometry()
    : meshMode_(MeshMode::CORNERS_ONLY) {
    resetCorners();
    uCoords_ = evenCoords(DEFAULT_MESH_RESOLUTION);
    vCoords_ = evenCoords(DEFAULT_MESH_RESOLUTION);
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

std::vector<float> QuadGeometry::evenCoords(int n) {
    n = std::clamp(n, MIN_MESH_RESOLUTION, MAX_MESH_RESOLUTION);
    std::vector<float> coords(static_cast<size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) {
        coords[static_cast<size_t>(i)] = static_cast<float>(i) / n;
    }
    return coords;
}

void QuadGeometry::setUniformGrid(int n) {
    n = std::clamp(n, MIN_MESH_RESOLUTION, MAX_MESH_RESOLUTION);
    // Rebuild even grids and clear offsets (matches the legacy resolution reset).
    if (getColumns() == n && getRows() == n && isUniform()) {
        return;  // already this uniform grid
    }
    uCoords_ = evenCoords(n);
    vCoords_ = evenCoords(n);
    initializeMeshVertices();
}

void QuadGeometry::setGridCoords(const std::vector<float>& u,
                                 const std::vector<float>& v) {
    if (u.size() < 2 || v.size() < 2) return;
    uCoords_ = u;
    vCoords_ = v;
    // Force pinned endpoints; assume the caller supplied sorted interior values.
    uCoords_.front() = 0.0f; uCoords_.back() = 1.0f;
    vCoords_.front() = 0.0f; vCoords_.back() = 1.0f;
    initializeMeshVertices();
}

bool QuadGeometry::isUniform() const {
    if (uCoords_.size() != vCoords_.size()) return false;
    const int n = getColumns();
    constexpr float EPS = 1e-5f;
    for (int i = 0; i <= n; ++i) {
        const float expected = static_cast<float>(i) / n;
        if (std::abs(uCoords_[i] - expected) > EPS) return false;
        if (std::abs(vCoords_[i] - expected) > EPS) return false;
    }
    return true;
}

float QuadGeometry::getUCoord(int col) const {
    if (col >= 0 && col < static_cast<int>(uCoords_.size())) return uCoords_[col];
    return 0.0f;
}

float QuadGeometry::getVCoord(int row) const {
    if (row >= 0 && row < static_cast<int>(vCoords_.size())) return vCoords_[row];
    return 0.0f;
}

// Insert a division at p along one axis, interpolating the per-vertex offsets of
// the new line from its two neighbours. Shared by insertU/VDivision via a lambda
// that knows whether we are inserting a column (U) or a row (V).
void QuadGeometry::insertUDivision(float u) {
    if (u <= 0.0f || u >= 1.0f) return;
    // Reject near-duplicates.
    for (float c : uCoords_) {
        if (std::abs(c - u) < 1e-4f) return;
    }
    // Find insertion column index k (uCoords_[k-1] < u < uCoords_[k]).
    int k = static_cast<int>(std::upper_bound(uCoords_.begin(), uCoords_.end(), u) -
                             uCoords_.begin());
    const int oldCols = vertsPerRow();
    const int rows = vertsPerCol();
    const float t = (u - uCoords_[k - 1]) / (uCoords_[k] - uCoords_[k - 1]);

    std::vector<Vec2> next(static_cast<size_t>(rows) * (oldCols + 1));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < oldCols; ++c) {
            int dstCol = (c < k) ? c : c + 1;
            next[static_cast<size_t>(r) * (oldCols + 1) + dstCol] =
                meshVertices_[static_cast<size_t>(r) * oldCols + c];
        }
        // Interpolated offset for the new column.
        const Vec2 left = meshVertices_[static_cast<size_t>(r) * oldCols + (k - 1)];
        const Vec2 right = meshVertices_[static_cast<size_t>(r) * oldCols + k];
        next[static_cast<size_t>(r) * (oldCols + 1) + k] = glm::mix(left, right, t);
    }
    uCoords_.insert(uCoords_.begin() + k, u);
    meshVertices_ = std::move(next);
}

void QuadGeometry::insertVDivision(float v) {
    if (v <= 0.0f || v >= 1.0f) return;
    for (float c : vCoords_) {
        if (std::abs(c - v) < 1e-4f) return;
    }
    int k = static_cast<int>(std::upper_bound(vCoords_.begin(), vCoords_.end(), v) -
                             vCoords_.begin());
    const int cols = vertsPerRow();
    const int oldRows = vertsPerCol();
    const float t = (v - vCoords_[k - 1]) / (vCoords_[k] - vCoords_[k - 1]);

    std::vector<Vec2> next(static_cast<size_t>(oldRows + 1) * cols);
    for (int r = 0; r < oldRows; ++r) {
        int dstRow = (r < k) ? r : r + 1;
        for (int c = 0; c < cols; ++c) {
            next[static_cast<size_t>(dstRow) * cols + c] =
                meshVertices_[static_cast<size_t>(r) * cols + c];
        }
    }
    for (int c = 0; c < cols; ++c) {
        const Vec2 above = meshVertices_[static_cast<size_t>(k - 1) * cols + c];
        const Vec2 below = meshVertices_[static_cast<size_t>(k) * cols + c];
        next[static_cast<size_t>(k) * cols + c] = glm::mix(above, below, t);
    }
    vCoords_.insert(vCoords_.begin() + k, v);
    meshVertices_ = std::move(next);
}

void QuadGeometry::removeUDivision(int col) {
    // Only interior lines (endpoints 0 and last stay).
    if (col <= 0 || col >= static_cast<int>(uCoords_.size()) - 1) return;
    if (uCoords_.size() <= static_cast<size_t>(MIN_MESH_RESOLUTION) + 1) return;
    const int oldCols = vertsPerRow();
    const int rows = vertsPerCol();
    std::vector<Vec2> next(static_cast<size_t>(rows) * (oldCols - 1));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0, dst = 0; c < oldCols; ++c) {
            if (c == col) continue;
            next[static_cast<size_t>(r) * (oldCols - 1) + dst++] =
                meshVertices_[static_cast<size_t>(r) * oldCols + c];
        }
    }
    uCoords_.erase(uCoords_.begin() + col);
    meshVertices_ = std::move(next);
}

void QuadGeometry::removeVDivision(int row) {
    if (row <= 0 || row >= static_cast<int>(vCoords_.size()) - 1) return;
    if (vCoords_.size() <= static_cast<size_t>(MIN_MESH_RESOLUTION) + 1) return;
    const int cols = vertsPerRow();
    const int oldRows = vertsPerCol();
    std::vector<Vec2> next(static_cast<size_t>(oldRows - 1) * cols);
    for (int r = 0, dst = 0; r < oldRows; ++r) {
        if (r == row) continue;
        for (int c = 0; c < cols; ++c) {
            next[static_cast<size_t>(dst) * cols + c] =
                meshVertices_[static_cast<size_t>(r) * cols + c];
        }
        ++dst;
    }
    vCoords_.erase(vCoords_.begin() + row);
    meshVertices_ = std::move(next);
}

void QuadGeometry::setMeshVertices(const std::vector<Vec2>& vertices) {
    int expectedCount = getVertexCount();
    if (static_cast<int>(vertices.size()) == expectedCount) {
        meshVertices_ = vertices;
    }
}

void QuadGeometry::setMeshVertex(int row, int col, const Vec2& offset) {
    setMeshVertexByIndex(vertexIndex(row, col), offset);
}

void QuadGeometry::setMeshVertexByIndex(int index, const Vec2& offset) {
    if (index >= 0 && index < static_cast<int>(meshVertices_.size())) {
        meshVertices_[index] = offset;
    }
}

Vec2 QuadGeometry::getMeshVertex(int row, int col) const {
    return getMeshVertexByIndex(vertexIndex(row, col));
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
    return vertsPerRow() * vertsPerCol();
}

void QuadGeometry::initializeMeshVertices() {
    meshVertices_.assign(static_cast<size_t>(getVertexCount()), Vec2(0.0f));
}

Vec2 QuadGeometry::transformUVToPosition(const Vec2& uv) const {
    // Bilinear interpolation
    Vec2 top = glm::mix(corners_[0], corners_[1], uv.x);
    Vec2 bottom = glm::mix(corners_[3], corners_[2], uv.x);
    return glm::mix(top, bottom, uv.y);
}

std::vector<Vec2> QuadGeometry::generateMeshPositions() const {
    std::vector<Vec2> positions(static_cast<size_t>(getVertexCount()));
    const bool edit = (meshMode_ == MeshMode::MESH_EDIT);

    for (int row = 0; row < vertsPerCol(); ++row) {
        for (int col = 0; col < vertsPerRow(); ++col) {
            Vec2 pos = transformUVToPosition(Vec2(uCoords_[col], vCoords_[row]));
            const int index = vertexIndex(row, col);
            if (edit) pos += meshVertices_[index];
            positions[index] = pos;
        }
    }
    return positions;
}

std::vector<Vec2> QuadGeometry::generateBilinearPositions() const {
    std::vector<Vec2> positions(static_cast<size_t>(getVertexCount()));
    for (int row = 0; row < vertsPerCol(); ++row) {
        for (int col = 0; col < vertsPerRow(); ++col) {
            positions[vertexIndex(row, col)] =
                transformUVToPosition(Vec2(uCoords_[col], vCoords_[row]));
        }
    }
    return positions;
}

} // namespace quadmapper
