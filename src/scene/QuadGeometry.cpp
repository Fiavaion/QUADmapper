#include "QuadGeometry.hpp"
#include <algorithm>
#include <cmath>

namespace quadmapper {

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

// A quad's corner surface B, set up once so a whole tessellation solves the homography
// and gathers the bezier controls a single time.
class CornerSurface {
public:
    // controls: the 8 effective bezier control points, or nullptr for straight edges.
    // mode LINEAR always uses the bilinear patch (ADR-058).
    CornerSurface(const std::array<Vec2, 4>& corners, const std::array<Vec2, 8>* controls,
                  WarpMode mode)
        : corners_(corners), controls_(controls) {
        projective_ = mode == WarpMode::PERSPECTIVE && squareToQuad(corners_, hm_);
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

    Vec2 point(float u, float v) const { return sample(u, v).position; }

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

// Cubic Hermite basis at s in [0,1]: values at the two ends (v0, v1) and tangents
// at the two ends (t0, t1).
struct Hermite {
    float v0, t0, v1, t1;
};

Hermite hermite(float s) {
    const float s2 = s * s;
    const float s3 = s2 * s;
    return {2.0f * s3 - 3.0f * s2 + 1.0f, s3 - 2.0f * s2 + s, -2.0f * s3 + 3.0f * s2, s3 - s2};
}

// Cell of the sorted grid lines that holds t, and t's local parameter in it.
int cellOf(const std::vector<float>& grid, float t, float& local) {
    const int cells = static_cast<int>(grid.size()) - 1;
    int cell = static_cast<int>(std::upper_bound(grid.begin(), grid.end(), t) - grid.begin()) - 1;
    cell = std::clamp(cell, 0, cells - 1);
    local = (t - grid[cell]) / (grid[cell + 1] - grid[cell]);
    return cell;
}

// The residual R (ADR-049 §1): per cell a bicubic Hermite patch through the control
// vertices' residuals, with the effective one-sided tangents (explicit, or
// Catmull-Rom from the neighbours) and a twist shared by the four cells around each
// vertex. Values and derivatives on a shared cell edge come only from that edge's two
// vertices, so the surface is C0 everywhere and C1 wherever the two sides' tangents
// agree (always, with auto tangents). With every tangent auto this is the
// tensor-product Catmull-Rom surface.
class ResidualField {
public:
    ResidualField(const std::vector<float>& u, const std::vector<float>& v,
                  const std::vector<Vec2>& residuals, const std::vector<VertexTangents>& tangents)
        : u_(u), v_(v), r_(residuals), cols_(static_cast<int>(u.size())),
          rows_(static_cast<int>(v.size())) {
        const size_t count = r_.size();
        tangent_.resize(count * GRID_HANDLE_COUNT);
        for (int row = 0; row < rows_; ++row) {
            for (int col = 0; col < cols_; ++col) {
                const int k = row * cols_ + col;
                const int cl = std::max(col - 1, 0), ch = std::min(col + 1, cols_ - 1);
                const int rl = std::max(row - 1, 0), rh = std::min(row + 1, rows_ - 1);
                const Vec2 autoU = (at(row, ch) - at(row, cl)) / (u_[ch] - u_[cl]);
                const Vec2 autoV = (at(rh, col) - at(rl, col)) / (v_[rh] - v_[rl]);
                const VertexTangents& t = tangents[static_cast<size_t>(k)];
                for (int h = 0; h < GRID_HANDLE_COUNT; ++h) {
                    const Vec2 automatic = (h < GRID_HANDLE_V_BEFORE) ? autoU : autoV;
                    tangent_[static_cast<size_t>(k) * GRID_HANDLE_COUNT + h] =
                        t.isExplicit(h) ? t.derivative[static_cast<size_t>(h)] : automatic;
                }
            }
        }
        // Twist: the cross derivative, from differencing each axis's tangents along the
        // other axis (the two agree for auto tangents; average them otherwise).
        twist_.resize(count);
        for (int row = 0; row < rows_; ++row) {
            for (int col = 0; col < cols_; ++col) {
                const int cl = std::max(col - 1, 0), ch = std::min(col + 1, cols_ - 1);
                const int rl = std::max(row - 1, 0), rh = std::min(row + 1, rows_ - 1);
                const Vec2 dTuDv = (meanU(rh, col) - meanU(rl, col)) / (v_[rh] - v_[rl]);
                const Vec2 dTvDu = (meanV(row, ch) - meanV(row, cl)) / (u_[ch] - u_[cl]);
                twist_[static_cast<size_t>(row * cols_ + col)] = 0.5f * (dTuDv + dTvDu);
            }
        }
    }

    Vec2 tangent(int index, int handle) const {
        return tangent_[static_cast<size_t>(index) * GRID_HANDLE_COUNT + handle];
    }

    Vec2 eval(float u, float v) const {
        float s = 0.0f, t = 0.0f;
        const int i = cellOf(u_, u, s);
        const int j = cellOf(v_, v, t);
        return evalCell(i, j, hermite(s), hermite(t));
    }

    // R in cell (column i, row j) from the Hermite bases along u (hu) and v (hv).
    Vec2 evalCell(int i, int j, const Hermite& hu, const Hermite& hv) const {
        const float wu = u_[i + 1] - u_[i];
        const float wv = v_[j + 1] - v_[j];
        const int k00 = j * cols_ + i, k10 = k00 + 1;  // row j
        const int k01 = k00 + cols_, k11 = k01 + 1;    // row j + 1
        const Vec2 value = hv.v0 * (hu.v0 * at(k00) + hu.v1 * at(k10)) +
                           hv.v1 * (hu.v0 * at(k01) + hu.v1 * at(k11));
        // Each corner contributes the tangent of its side facing into the cell.
        const Vec2 alongU =
            hv.v0 * (hu.t0 * tangent(k00, GRID_HANDLE_U_AFTER) + hu.t1 * tangent(k10, GRID_HANDLE_U_BEFORE)) +
            hv.v1 * (hu.t0 * tangent(k01, GRID_HANDLE_U_AFTER) + hu.t1 * tangent(k11, GRID_HANDLE_U_BEFORE));
        const Vec2 alongV =
            hv.t0 * (hu.v0 * tangent(k00, GRID_HANDLE_V_AFTER) + hu.v1 * tangent(k10, GRID_HANDLE_V_AFTER)) +
            hv.t1 * (hu.v0 * tangent(k01, GRID_HANDLE_V_BEFORE) + hu.v1 * tangent(k11, GRID_HANDLE_V_BEFORE));
        const Vec2 cross = hv.t0 * (hu.t0 * twist(k00) + hu.t1 * twist(k10)) +
                           hv.t1 * (hu.t0 * twist(k01) + hu.t1 * twist(k11));
        return value + wu * alongU + wv * alongV + (wu * wv) * cross;
    }

private:
    Vec2 at(int k) const { return r_[static_cast<size_t>(k)]; }
    Vec2 at(int row, int col) const { return at(row * cols_ + col); }
    Vec2 twist(int k) const { return twist_[static_cast<size_t>(k)]; }

    // The vertex's tangent along one axis: the mean of its sides inside the grid.
    Vec2 meanU(int row, int col) const {
        const int k = row * cols_ + col;
        if (col == 0) return tangent(k, GRID_HANDLE_U_AFTER);
        if (col == cols_ - 1) return tangent(k, GRID_HANDLE_U_BEFORE);
        return 0.5f * (tangent(k, GRID_HANDLE_U_BEFORE) + tangent(k, GRID_HANDLE_U_AFTER));
    }
    Vec2 meanV(int row, int col) const {
        const int k = row * cols_ + col;
        if (row == 0) return tangent(k, GRID_HANDLE_V_AFTER);
        if (row == rows_ - 1) return tangent(k, GRID_HANDLE_V_BEFORE);
        return 0.5f * (tangent(k, GRID_HANDLE_V_BEFORE) + tangent(k, GRID_HANDLE_V_AFTER));
    }

    const std::vector<float>& u_;
    const std::vector<float>& v_;
    const std::vector<Vec2>& r_;
    int cols_;
    int rows_;
    std::vector<Vec2> tangent_;  // GRID_HANDLE_COUNT per vertex
    std::vector<Vec2> twist_;
};

}  // namespace

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

// Insert a division at u. Each new control vertex takes the residual R already has at
// its parameters, so it lands on the current surface; its tangents start automatic.
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

    std::vector<Vec2> next(static_cast<size_t>(rows) * (oldCols + 1));
    std::vector<VertexTangents> nextTangents(next.size());
    {
        const ResidualField residual(uCoords_, vCoords_, meshVertices_, tangents_);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < oldCols; ++c) {
                const size_t dst = static_cast<size_t>(r) * (oldCols + 1) + ((c < k) ? c : c + 1);
                const size_t src = static_cast<size_t>(r) * oldCols + c;
                next[dst] = meshVertices_[src];
                nextTangents[dst] = tangents_[src];
            }
            next[static_cast<size_t>(r) * (oldCols + 1) + k] = residual.eval(u, vCoords_[r]);
        }
    }
    uCoords_.insert(uCoords_.begin() + k, u);
    meshVertices_ = std::move(next);
    tangents_ = std::move(nextTangents);
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

    std::vector<Vec2> next(static_cast<size_t>(oldRows + 1) * cols);
    std::vector<VertexTangents> nextTangents(next.size());
    {
        const ResidualField residual(uCoords_, vCoords_, meshVertices_, tangents_);
        for (int r = 0; r < oldRows; ++r) {
            const int dstRow = (r < k) ? r : r + 1;
            for (int c = 0; c < cols; ++c) {
                const size_t dst = static_cast<size_t>(dstRow) * cols + c;
                const size_t src = static_cast<size_t>(r) * cols + c;
                next[dst] = meshVertices_[src];
                nextTangents[dst] = tangents_[src];
            }
        }
        for (int c = 0; c < cols; ++c) {
            next[static_cast<size_t>(k) * cols + c] = residual.eval(uCoords_[c], v);
        }
    }
    vCoords_.insert(vCoords_.begin() + k, v);
    meshVertices_ = std::move(next);
    tangents_ = std::move(nextTangents);
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
    std::vector<VertexTangents> nextTangents(next.size());
    for (int r = 0; r < rows; ++r) {
        for (int c = 0, dst = 0; c < oldCols; ++c) {
            if (c == col) continue;
            const size_t to = static_cast<size_t>(r) * (oldCols - 1) + dst++;
            const size_t from = static_cast<size_t>(r) * oldCols + c;
            next[to] = meshVertices_[from];
            nextTangents[to] = tangents_[from];
        }
    }
    uCoords_.erase(uCoords_.begin() + col);
    meshVertices_ = std::move(next);
    tangents_ = std::move(nextTangents);
}

void QuadGeometry::removeVDivision(int row) {
    if (row <= 0 || row >= static_cast<int>(vCoords_.size()) - 1) return;
    if (vCoords_.size() <= static_cast<size_t>(MIN_MESH_RESOLUTION) + 1) return;
    const int cols = vertsPerRow();
    const int oldRows = vertsPerCol();
    std::vector<Vec2> next(static_cast<size_t>(oldRows - 1) * cols);
    std::vector<VertexTangents> nextTangents(next.size());
    for (int r = 0, dst = 0; r < oldRows; ++r) {
        if (r == row) continue;
        for (int c = 0; c < cols; ++c) {
            const size_t to = static_cast<size_t>(dst) * cols + c;
            const size_t from = static_cast<size_t>(r) * cols + c;
            next[to] = meshVertices_[from];
            nextTangents[to] = tangents_[from];
        }
        ++dst;
    }
    vCoords_.erase(vCoords_.begin() + row);
    meshVertices_ = std::move(next);
    tangents_ = std::move(nextTangents);
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
    std::fill(tangents_.begin(), tangents_.end(), VertexTangents{});
}

bool QuadGeometry::hasMeshModifications() const {
    constexpr float EPSILON = 1e-6f;
    for (const auto& offset : meshVertices_) {
        if (std::abs(offset.x) > EPSILON || std::abs(offset.y) > EPSILON) {
            return true;
        }
    }
    return hasExplicitTangents();
}

int QuadGeometry::getVertexCount() const {
    return vertsPerRow() * vertsPerCol();
}

void QuadGeometry::initializeMeshVertices() {
    meshVertices_.assign(static_cast<size_t>(getVertexCount()), Vec2(0.0f));
    tangents_.assign(meshVertices_.size(), VertexTangents{});
}

void QuadGeometry::convertBilinearOffsetsToResiduals() {
    const std::array<Vec2, 8> controls = bezierControls();
    const CornerSurface base(corners_, bezierEnabled_ ? &controls : nullptr, warpMode_);
    for (int row = 0; row < vertsPerCol(); ++row) {
        for (int col = 0; col < vertsPerRow(); ++col) {
            const Vec2 uv(uCoords_[col], vCoords_[row]);
            Vec2& offset = meshVertices_[static_cast<size_t>(vertexIndex(row, col))];
            offset = transformUVToPosition(uv) + offset - base.point(uv.x, uv.y);
        }
    }
}

void QuadGeometry::setWarpMode(WarpMode mode) {
    if (mode == warpMode_) return;
    if (!hasMeshModifications()) {
        warpMode_ = mode;
        return;
    }
    // Keep each control vertex's absolute position: residual = (old B + residual) - new B.
    const std::array<Vec2, 8> controls = bezierControls();
    const std::array<Vec2, 8>* curves = bezierEnabled_ ? &controls : nullptr;
    const CornerSurface before(corners_, curves, warpMode_);
    const CornerSurface after(corners_, curves, mode);
    for (int row = 0; row < vertsPerCol(); ++row) {
        for (int col = 0; col < vertsPerRow(); ++col) {
            const float u = uCoords_[col], v = vCoords_[row];
            Vec2& offset = meshVertices_[static_cast<size_t>(vertexIndex(row, col))];
            offset += before.point(u, v) - after.point(u, v);
        }
    }
    warpMode_ = mode;
}

void QuadGeometry::setVertexTangents(const std::vector<VertexTangents>& tangents) {
    if (tangents.size() == tangents_.size()) {
        tangents_ = tangents;
    }
}

VertexTangents QuadGeometry::getVertexTangent(int index) const {
    if (index >= 0 && index < static_cast<int>(tangents_.size())) {
        return tangents_[static_cast<size_t>(index)];
    }
    return VertexTangents{};
}

void QuadGeometry::setVertexTangent(int index, const VertexTangents& tangents) {
    if (index >= 0 && index < static_cast<int>(tangents_.size())) {
        tangents_[static_cast<size_t>(index)] = tangents;
    }
}

bool QuadGeometry::hasExplicitTangents() const {
    return std::any_of(tangents_.begin(), tangents_.end(),
                       [](const VertexTangents& t) { return t.explicitMask != 0; });
}

unsigned QuadGeometry::availableHandles(int index) const {
    if (index < 0 || index >= getVertexCount()) return 0;
    const int row = index / vertsPerRow();
    const int col = index % vertsPerRow();
    unsigned mask = 0;
    if (col > 0) mask |= 1u << GRID_HANDLE_U_BEFORE;
    if (col < vertsPerRow() - 1) mask |= 1u << GRID_HANDLE_U_AFTER;
    if (row > 0) mask |= 1u << GRID_HANDLE_V_BEFORE;
    if (row < vertsPerCol() - 1) mask |= 1u << GRID_HANDLE_V_AFTER;
    return mask;
}

float QuadGeometry::handleSpan(int index, int handle, float& sign) const {
    const int row = index / vertsPerRow();
    const int col = index % vertsPerRow();
    switch (handle) {
        case GRID_HANDLE_U_BEFORE: sign = -1.0f; return uCoords_[col] - uCoords_[col - 1];
        case GRID_HANDLE_U_AFTER:  sign = 1.0f;  return uCoords_[col + 1] - uCoords_[col];
        case GRID_HANDLE_V_BEFORE: sign = -1.0f; return vCoords_[row] - vCoords_[row - 1];
        default:                   sign = 1.0f;  return vCoords_[row + 1] - vCoords_[row];
    }
}

Vec2 QuadGeometry::baseDerivative(int index, bool alongU) const {
    // Central difference of B, one-sided at the quad's edges.
    constexpr float H = 1e-3f;
    const std::array<Vec2, 8> controls = bezierControls();
    const CornerSurface base(corners_, bezierEnabled_ ? &controls : nullptr, warpMode_);
    const float u = uCoords_[static_cast<size_t>(index % vertsPerRow())];
    const float v = vCoords_[static_cast<size_t>(index / vertsPerRow())];
    if (alongU) {
        const float lo = std::max(u - H, 0.0f), hi = std::min(u + H, 1.0f);
        return (base.point(hi, v) - base.point(lo, v)) / (hi - lo);
    }
    const float lo = std::max(v - H, 0.0f), hi = std::min(v + H, 1.0f);
    return (base.point(u, hi) - base.point(u, lo)) / (hi - lo);
}

Vec2 QuadGeometry::meshedVertex(int index) const {
    return basePoint(uCoords_[static_cast<size_t>(index % vertsPerRow())],
                     vCoords_[static_cast<size_t>(index / vertsPerRow())]) +
           meshVertices_[static_cast<size_t>(index)];
}

Vec2 QuadGeometry::gridHandlePosition(int index, int handle) const {
    if (index < 0 || index >= getVertexCount()) return Vec2(0.0f);
    const Vec2 vertex = meshedVertex(index);
    if (!(availableHandles(index) & (1u << handle))) return vertex;
    const ResidualField residual(uCoords_, vCoords_, meshVertices_, tangents_);
    const Vec2 derivative =
        baseDerivative(index, handle < GRID_HANDLE_V_BEFORE) + residual.tangent(index, handle);
    float sign = 1.0f;
    const float span = handleSpan(index, handle, sign);
    return vertex + (sign * span / 3.0f) * derivative;
}

void QuadGeometry::setGridHandlePosition(int index, int handle, const Vec2& pos) {
    if (!(availableHandles(index) & (1u << handle))) return;
    const Vec2 vertex = meshedVertex(index);
    float sign = 1.0f;
    const float span = handleSpan(index, handle, sign);
    const Vec2 surfaceDerivative = (3.0f * sign / span) * (pos - vertex);
    VertexTangents& t = tangents_[static_cast<size_t>(index)];
    t.derivative[static_cast<size_t>(handle)] =
        surfaceDerivative - baseDerivative(index, handle < GRID_HANDLE_V_BEFORE);
    t.explicitMask |= 1u << handle;
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

Vec2 QuadGeometry::basePoint(float u, float v) const {
    const std::array<Vec2, 8> controls = bezierControls();
    return CornerSurface(corners_, bezierEnabled_ ? &controls : nullptr, warpMode_).point(u, v);
}

bool QuadGeometry::hasResidualSurface() const {
    return meshMode_ == MeshMode::MESH_EDIT && hasMeshModifications();
}

Vec2 QuadGeometry::surfacePoint(float u, float v) const {
    const Vec2 base = basePoint(u, v);
    if (!hasResidualSurface()) return base;
    return base + ResidualField(uCoords_, vCoords_, meshVertices_, tangents_).eval(u, v);
}

std::vector<QuadGeometry::SurfaceSample> QuadGeometry::sampleSurface(
    const std::vector<float>& us, const std::vector<float>& vs) const {
    const std::array<Vec2, 8> controls = bezierControls();
    const CornerSurface base(corners_, bezierEnabled_ ? &controls : nullptr, warpMode_);
    std::vector<SurfaceSample> samples;
    samples.reserve(us.size() * vs.size());
    for (float v : vs) {
        for (float u : us) {
            samples.push_back(base.sample(u, v));
        }
    }
    if (!hasResidualSurface()) return samples;

    // Add R, finding each sample's cell and Hermite basis once per row and column.
    const ResidualField residual(uCoords_, vCoords_, meshVertices_, tangents_);
    std::vector<int> cellU(us.size());
    std::vector<Hermite> basisU(us.size());
    for (size_t c = 0; c < us.size(); ++c) {
        float s = 0.0f;
        cellU[c] = cellOf(uCoords_, us[c], s);
        basisU[c] = hermite(s);
    }
    for (size_t r = 0; r < vs.size(); ++r) {
        float t = 0.0f;
        const int cellV = cellOf(vCoords_, vs[r], t);
        const Hermite basisV = hermite(t);
        for (size_t c = 0; c < us.size(); ++c) {
            samples[r * us.size() + c].position +=
                residual.evalCell(cellU[c], cellV, basisU[c], basisV);
        }
    }
    return samples;
}

std::vector<float> QuadGeometry::tessellationParams(const std::vector<float>& grid,
                                                    bool residual) {
    std::vector<float> params;
    if (!residual) {
        params.reserve(RENDER_MIN_SEGMENTS + 1);
        for (int i = 0; i <= RENDER_MIN_SEGMENTS; ++i) {
            params.push_back(static_cast<float>(i) / RENDER_MIN_SEGMENTS);
        }
        return params;
    }
    const int cells = static_cast<int>(grid.size()) - 1;
    const int perCell = std::max(RENDER_MIN_SEGMENTS_PER_CELL,
                                 (RENDER_MIN_SEGMENTS + cells - 1) / cells);
    params.reserve(static_cast<size_t>(cells) * perCell + 1);
    for (int c = 0; c < cells; ++c) {
        const float lo = grid[static_cast<size_t>(c)];
        const float width = grid[static_cast<size_t>(c) + 1] - lo;
        params.push_back(lo);  // the control line itself, exactly
        for (int s = 1; s < perCell; ++s) {
            params.push_back(lo + width * static_cast<float>(s) / perCell);
        }
    }
    params.push_back(grid.back());
    return params;
}

std::vector<float> QuadGeometry::tessellationU() const {
    return tessellationParams(uCoords_, hasResidualSurface());
}

std::vector<float> QuadGeometry::tessellationV() const {
    return tessellationParams(vCoords_, hasResidualSurface());
}

Vec2 QuadGeometry::surfaceParamsAt(const Vec2& p) const {
    const std::vector<float> us = tessellationU();
    const std::vector<float> vs = tessellationV();
    const std::vector<SurfaceSample> samples = sampleSurface(us, vs);
    const size_t line = us.size();

    // Triangles as the renderer draws them: (tl, bl, tr) and (tr, bl, br) per cell.
    struct Corner { Vec2 pos; Vec2 uv; float q; };
    auto corner = [&](size_t r, size_t c) {
        const SurfaceSample& s = samples[r * line + c];
        return Corner{s.position, Vec2(us[c], vs[r]), s.q};
    };
    auto inside = [&](const Corner& a, const Corner& b, const Corner& c, Vec2& uv) {
        const float minX = std::min({a.pos.x, b.pos.x, c.pos.x});
        const float maxX = std::max({a.pos.x, b.pos.x, c.pos.x});
        const float minY = std::min({a.pos.y, b.pos.y, c.pos.y});
        const float maxY = std::max({a.pos.y, b.pos.y, c.pos.y});
        if (p.x < minX || p.x > maxX || p.y < minY || p.y > maxY) return false;
        const Vec2 e0 = b.pos - a.pos, e1 = c.pos - a.pos, d = p - a.pos;
        const float area = e0.x * e1.y - e0.y * e1.x;
        if (std::fabs(area) < 1e-12f) return false;
        const float wb = (d.x * e1.y - d.y * e1.x) / area;
        const float wc = (e0.x * d.y - e0.y * d.x) / area;
        const float wa = 1.0f - wb - wc;
        constexpr float EDGE = -1e-6f;  // points on a shared edge belong to both
        if (wa < EDGE || wb < EDGE || wc < EDGE) return false;
        // Projective interpolation, as quad.frag divides (u*q, v*q) by q.
        const float q = wa * a.q + wb * b.q + wc * c.q;
        uv = (wa * a.q * a.uv + wb * b.q * b.uv + wc * c.q * c.uv) / q;
        uv = glm::clamp(uv, Vec2(0.0f), Vec2(1.0f));
        return true;
    };
    Vec2 uv;
    for (size_t r = 0; r + 1 < vs.size(); ++r) {
        for (size_t c = 0; c + 1 < line; ++c) {
            const Corner tl = corner(r, c), tr = corner(r, c + 1);
            const Corner bl = corner(r + 1, c), br = corner(r + 1, c + 1);
            if (inside(tl, bl, tr, uv) || inside(tr, bl, br, uv)) return uv;
        }
    }
    return Vec2(-1.0f);
}

std::vector<Vec2> QuadGeometry::generateMeshPositions() const {
    std::vector<Vec2> positions = generateBasePositions();
    if (meshMode_ == MeshMode::MESH_EDIT) {
        for (size_t i = 0; i < positions.size(); ++i) {
            positions[i] += meshVertices_[i];
        }
    }
    return positions;
}

std::vector<Vec2> QuadGeometry::generateBasePositions() const {
    const std::array<Vec2, 8> controls = bezierControls();
    const CornerSurface base(corners_, bezierEnabled_ ? &controls : nullptr, warpMode_);
    std::vector<Vec2> positions(static_cast<size_t>(getVertexCount()));
    for (int row = 0; row < vertsPerCol(); ++row) {
        for (int col = 0; col < vertsPerRow(); ++col) {
            positions[static_cast<size_t>(vertexIndex(row, col))] =
                base.point(uCoords_[col], vCoords_[row]);
        }
    }
    return positions;
}

} // namespace quadmapper
