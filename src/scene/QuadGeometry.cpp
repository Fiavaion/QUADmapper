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
    bezierOffsets_.fill(Vec2(0.0f));  // glm leaves members uninitialized otherwise
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
int QuadGeometry::insertUDivision(float u) {
    if (u <= 0.0f || u >= 1.0f) return -1;
    // Reject near-duplicates.
    for (float c : uCoords_) {
        if (std::abs(c - u) < 1e-4f) return -1;
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
    return k;
}

int QuadGeometry::insertVDivision(float v) {
    if (v <= 0.0f || v >= 1.0f) return -1;
    for (float c : vCoords_) {
        if (std::abs(c - v) < 1e-4f) return -1;
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
    return k;
}

void QuadGeometry::moveUDivision(int col, float u) {
    // Only interior lines move; clamp strictly between the two neighbours.
    if (col <= 0 || col >= static_cast<int>(uCoords_.size()) - 1) return;
    constexpr float GAP = 1e-3f;
    const float lo = uCoords_[col - 1] + GAP;
    const float hi = uCoords_[col + 1] - GAP;
    if (hi < lo) return;
    uCoords_[col] = std::clamp(u, lo, hi);
}

void QuadGeometry::moveVDivision(int row, float v) {
    if (row <= 0 || row >= static_cast<int>(vCoords_.size()) - 1) return;
    constexpr float GAP = 1e-3f;
    const float lo = vCoords_[row - 1] + GAP;
    const float hi = vCoords_[row + 1] - GAP;
    if (hi < lo) return;
    vCoords_[row] = std::clamp(v, lo, hi);
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

Vec2 QuadGeometry::inverseBilinear(const std::array<Vec2, 4>& c, const Vec2& p) {
    auto cross2 = [](const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; };
    const Vec2 a = c[0], b = c[1], cc = c[2], d = c[3];  // TL, TR, BR, BL
    const Vec2 e = b - a, f = d - a, g = a - b + cc - d, h = p - a;
    const float k2 = cross2(g, f);
    const float k1 = cross2(e, f) + cross2(h, g);
    const float k0 = cross2(h, e);

    float v;
    if (std::fabs(k2) < 1e-7f) {                 // affine (parallelogram) case
        if (std::fabs(k1) < 1e-9f) return Vec2(-1.0f);
        v = -k0 / k1;
    } else {
        float disc = k1 * k1 - 4.0f * k0 * k2;
        if (disc < 0.0f) return Vec2(-1.0f);
        disc = std::sqrt(disc);
        const float v1 = (-k1 - disc) / (2.0f * k2);
        const float v2 = (-k1 + disc) / (2.0f * k2);
        v = (v1 >= -0.001f && v1 <= 1.001f) ? v1 : v2;
    }
    // Recover u from whichever axis has the more stable denominator.
    const float du = e.x + g.x * v;
    const float dv = e.y + g.y * v;
    if (std::fabs(du) >= std::fabs(dv)) {
        if (std::fabs(du) < 1e-9f) return Vec2(-1.0f);
        return Vec2((h.x - f.x * v) / du, v);
    }
    if (std::fabs(dv) < 1e-9f) return Vec2(-1.0f);
    return Vec2((h.y - f.y * v) / dv, v);
}

Vec2 QuadGeometry::transformUVToPosition(const Vec2& uv) const {
    // Bilinear interpolation
    Vec2 top = glm::mix(corners_[0], corners_[1], uv.x);
    Vec2 bottom = glm::mix(corners_[3], corners_[2], uv.x);
    return glm::mix(top, bottom, uv.y);
}

std::array<Vec2, 8> QuadGeometry::bezierControls() const {
    std::array<Vec2, 8> controls;
    for (int edge = 0; edge < 4; ++edge) {
        controls[edge * 2] = getBezierControl(edge, 0);
        controls[edge * 2 + 1] = getBezierControl(edge, 1);
    }
    return controls;
}

Vec2 QuadGeometry::getBezierControl(int edge, int which) const {
    const Vec2& a = corners_[edge];
    const Vec2& b = corners_[(edge + 1) % 4];
    const float f = (which == 0) ? (1.0f / 3.0f) : (2.0f / 3.0f);
    return glm::mix(a, b, f) + bezierOffsets_[edge * 2 + which];
}

void QuadGeometry::setBezierControl(int edge, int which, const Vec2& pos) {
    const Vec2& a = corners_[edge];
    const Vec2& b = corners_[(edge + 1) % 4];
    const float f = (which == 0) ? (1.0f / 3.0f) : (2.0f / 3.0f);
    bezierOffsets_[edge * 2 + which] = pos - glm::mix(a, b, f);
}

void QuadGeometry::resetBezier() {
    bezierOffsets_.fill(Vec2(0.0f));
    bezierEnabled_ = false;
}

bool QuadGeometry::hasBezierEdges() const {
    if (!bezierEnabled_) return false;
    for (const Vec2& o : bezierOffsets_) {
        if (o.x != 0.0f || o.y != 0.0f) return true;
    }
    return false;
}

namespace {

// Projective map of the unit square onto a quad (Heckbert's square-to-quad):
// x = (a u + b v + c) / w, y = (d u + e v + f) / w, with w = g u + h v + 1.
struct Homography {
    float a, b, c, d, e, f, g, h;
};

// Solves the square->quad map for corners [TL, TR, BR, BL] = (0,0),(1,0),(1,1),(0,1).
// Returns false unless w > 0 over the whole square, i.e. the corners form a convex
// quad (either winding); concave, self-intersecting or collapsed corners have no such
// map. w is affine in (u,v), so checking the four corner weights suffices. The margin
// is relative to the largest weight so it does not depend on which corner is TL.
bool squareToQuad(const std::array<Vec2, 4>& q, Homography& hm) {
    const float sx = q[0].x - q[1].x + q[2].x - q[3].x;
    const float sy = q[0].y - q[1].y + q[2].y - q[3].y;
    const float dx1 = q[1].x - q[2].x, dx2 = q[3].x - q[2].x;
    const float dy1 = q[1].y - q[2].y, dy2 = q[3].y - q[2].y;
    const float det = dx1 * dy2 - dx2 * dy1;
    if (std::fabs(det) < 1e-12f) return false;
    hm.g = (sx * dy2 - dx2 * sy) / det;
    hm.h = (dx1 * sy - sx * dy1) / det;
    hm.a = q[1].x - q[0].x + hm.g * q[1].x;
    hm.b = q[3].x - q[0].x + hm.h * q[3].x;
    hm.c = q[0].x;
    hm.d = q[1].y - q[0].y + hm.g * q[1].y;
    hm.e = q[3].y - q[0].y + hm.h * q[3].y;
    hm.f = q[0].y;
    const float w1 = 1.0f + hm.g, w2 = 1.0f + hm.g + hm.h, w3 = 1.0f + hm.h;
    constexpr float kMinRelativeW = 1e-4f;
    return std::min({1.0f, w1, w2, w3}) > kMinRelativeW * std::max({1.0f, w1, w2, w3});
}

// A quad's corner surface, set up once so a whole tessellation solves the homography
// and gathers the bezier controls a single time.
class CornerSurface {
public:
    // controls: the 8 effective bezier control points, or nullptr for straight edges.
    CornerSurface(const std::array<Vec2, 4>& corners, const std::array<Vec2, 8>* controls)
        : corners_(corners), controls_(controls) {
        projective_ = squareToQuad(corners_, hm_);
        if (projective_) w_ = {1.0f, 1.0f + hm_.g, 1.0f + hm_.g + hm_.h, 1.0f + hm_.h};
    }

    QuadGeometry::SurfaceSample sample(float u, float v) const {
        Vec2 base;
        float q = 1.0f;
        if (projective_) {
            q = 1.0f / (hm_.g * u + hm_.h * v + 1.0f);
            base = Vec2(hm_.a * u + hm_.b * v + hm_.c, hm_.d * u + hm_.e * v + hm_.f) * q;
        } else {
            base = glm::mix(glm::mix(corners_[0], corners_[1], u),
                            glm::mix(corners_[3], corners_[2], u), v);
        }
        if (!controls_) return {base, q};

        // Coons patch of the edge curves' deviations from their straight chords, added
        // over the base. The base walks each edge with a projective parameter, so each
        // deviation is sampled at that same chord parameter and the boundary lands
        // exactly on the bezier curve. With the bilinear fallback (all corner weights 1)
        // this is identical to the classic bilinearly-blended Coons patch. Edges run
        // TL->TR (0), TR->BR (1), BR->BL (2), BL->TL (3); 2 and 3 are reversed relative
        // to increasing u / v.
        const Vec2 dTop    = deviation(0, chordParam(0, 1, u));
        const Vec2 dRight  = deviation(1, chordParam(1, 2, v));
        const Vec2 dBottom = deviation(2, 1.0f - chordParam(3, 2, u));
        const Vec2 dLeft   = deviation(3, 1.0f - chordParam(0, 3, v));
        return {base + (1.0f - v) * dTop + v * dBottom + (1.0f - u) * dLeft + u * dRight, q};
    }

private:
    // Chord parameter of the base point at parameter t along the edge corner i -> j.
    float chordParam(int i, int j, float t) const {
        return w_[j] * t / ((1.0f - t) * w_[i] + t * w_[j]);
    }

    // Offset of bezier edge `edge` from its straight chord at parameter s.
    Vec2 deviation(int edge, float s) const {
        const Vec2& p0 = corners_[edge];
        const Vec2& p3 = corners_[(edge + 1) % 4];
        const Vec2& c1 = (*controls_)[edge * 2];
        const Vec2& c2 = (*controls_)[edge * 2 + 1];
        const float ms = 1.0f - s;
        const Vec2 curve = ms * ms * ms * p0 + 3.0f * ms * ms * s * c1
                         + 3.0f * ms * s * s * c2 + s * s * s * p3;
        return curve - glm::mix(p0, p3, s);
    }

    const std::array<Vec2, 4>& corners_;
    const std::array<Vec2, 8>* controls_;
    Homography hm_{};
    bool projective_ = false;
    std::array<float, 4> w_{1.0f, 1.0f, 1.0f, 1.0f};
};

}  // namespace

Vec2 QuadGeometry::surfacePoint(float u, float v) const {
    const std::array<Vec2, 8> controls = bezierControls();
    return CornerSurface(corners_, bezierEnabled_ ? &controls : nullptr)
        .sample(u, v).position;
}

std::vector<QuadGeometry::SurfaceSample> QuadGeometry::sampleSurface(int n) const {
    const std::array<Vec2, 8> controls = bezierControls();
    const CornerSurface surface(corners_, bezierEnabled_ ? &controls : nullptr);
    std::vector<SurfaceSample> samples;
    samples.reserve(static_cast<size_t>(n + 1) * (n + 1));
    for (int r = 0; r <= n; ++r) {
        for (int c = 0; c <= n; ++c) {
            samples.push_back(surface.sample(static_cast<float>(c) / n,
                                             static_cast<float>(r) / n));
        }
    }
    return samples;
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
