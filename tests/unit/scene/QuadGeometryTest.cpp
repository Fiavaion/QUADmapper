#include <gtest/gtest.h>
#include "scene/QuadGeometry.hpp"
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace quadmapper {
namespace test {

TEST(QuadGeometryTest, DefaultCornersForm40PercentSquare) {
    QuadGeometry geometry;

    auto corners = geometry.getCorners();

    // Default should be from (0.3, 0.3) to (0.7, 0.7) per ADR-028 (40% of canvas, centered)
    EXPECT_FLOAT_EQ(corners[0].x, 0.3f);  // TL
    EXPECT_FLOAT_EQ(corners[0].y, 0.3f);
    EXPECT_FLOAT_EQ(corners[1].x, 0.7f);  // TR
    EXPECT_FLOAT_EQ(corners[1].y, 0.3f);
    EXPECT_FLOAT_EQ(corners[2].x, 0.7f);  // BR
    EXPECT_FLOAT_EQ(corners[2].y, 0.7f);
    EXPECT_FLOAT_EQ(corners[3].x, 0.3f);  // BL
    EXPECT_FLOAT_EQ(corners[3].y, 0.7f);
}

TEST(QuadGeometryTest, SetCornerUpdatesPosition) {
    QuadGeometry geometry;

    geometry.setCorner(0, Vec2(0.15f, 0.25f));

    auto corners = geometry.getCorners();
    EXPECT_FLOAT_EQ(corners[0].x, 0.15f);
    EXPECT_FLOAT_EQ(corners[0].y, 0.25f);
}

TEST(QuadGeometryTest, SetCornersUpdatesAllPositions) {
    QuadGeometry geometry;

    std::array<Vec2, 4> newCorners = {
        Vec2(0.0f, 0.0f),
        Vec2(1.0f, 0.0f),
        Vec2(1.0f, 1.0f),
        Vec2(0.0f, 1.0f)
    };

    geometry.setCorners(newCorners);

    auto corners = geometry.getCorners();
    EXPECT_FLOAT_EQ(corners[0].x, 0.0f);
    EXPECT_FLOAT_EQ(corners[0].y, 0.0f);
    EXPECT_FLOAT_EQ(corners[2].x, 1.0f);
    EXPECT_FLOAT_EQ(corners[2].y, 1.0f);
}

TEST(QuadGeometryTest, ResetCornersRestoresToDefault) {
    QuadGeometry geometry;

    // Modify corners
    geometry.setCorner(0, Vec2(0.5f, 0.5f));

    // Reset
    geometry.resetCorners();

    auto corners = geometry.getCorners();
    EXPECT_FLOAT_EQ(corners[0].x, 0.3f);  // 40% default per ADR-028
    EXPECT_FLOAT_EQ(corners[0].y, 0.3f);
}

TEST(QuadGeometryTest, InvalidCornerIndexIgnored) {
    QuadGeometry geometry;
    auto originalCorners = geometry.getCorners();

    // These should be silently ignored, not throw
    geometry.setCorner(4, Vec2(0.5f, 0.5f));
    geometry.setCorner(-1, Vec2(0.5f, 0.5f));

    // Corners should be unchanged
    auto corners = geometry.getCorners();
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(corners[i].x, originalCorners[i].x);
        EXPECT_FLOAT_EQ(corners[i].y, originalCorners[i].y);
    }
}

TEST(QuadGeometryTest, BilinearInterpolation) {
    QuadGeometry geometry;

    // Set up a unit square for easy testing
    geometry.setCorners({
        Vec2(0.0f, 0.0f),
        Vec2(1.0f, 0.0f),
        Vec2(1.0f, 1.0f),
        Vec2(0.0f, 1.0f)
    });

    // Center should be (0.5, 0.5)
    Vec2 center = geometry.transformUVToPosition(Vec2(0.5f, 0.5f));
    EXPECT_NEAR(center.x, 0.5f, 0.001f);
    EXPECT_NEAR(center.y, 0.5f, 0.001f);

    // Top-left corner
    Vec2 topLeft = geometry.transformUVToPosition(Vec2(0.0f, 0.0f));
    EXPECT_NEAR(topLeft.x, 0.0f, 0.001f);
    EXPECT_NEAR(topLeft.y, 0.0f, 0.001f);

    // Bottom-right corner
    Vec2 bottomRight = geometry.transformUVToPosition(Vec2(1.0f, 1.0f));
    EXPECT_NEAR(bottomRight.x, 1.0f, 0.001f);
    EXPECT_NEAR(bottomRight.y, 1.0f, 0.001f);
}

TEST(QuadGeometryTest, DefaultMeshResolution) {
    QuadGeometry geometry;

    EXPECT_EQ(geometry.getMeshResolution(), DEFAULT_MESH_RESOLUTION);
}

TEST(QuadGeometryTest, SetMeshResolutionClampsToRange) {
    QuadGeometry geometry;

    geometry.setMeshResolution(MIN_MESH_RESOLUTION - 1);
    EXPECT_EQ(geometry.getMeshResolution(), MIN_MESH_RESOLUTION);

    geometry.setMeshResolution(MAX_MESH_RESOLUTION + 1);
    EXPECT_EQ(geometry.getMeshResolution(), MAX_MESH_RESOLUTION);

    geometry.setMeshResolution(8);
    EXPECT_EQ(geometry.getMeshResolution(), 8);
}

TEST(QuadGeometryTest, DefaultMeshModeIsCornersOnly) {
    QuadGeometry geometry;

    EXPECT_EQ(geometry.getMeshMode(), MeshMode::CORNERS_ONLY);
}

TEST(QuadGeometryTest, SetMeshModeUpdatesMode) {
    QuadGeometry geometry;

    geometry.setMeshMode(MeshMode::MESH_EDIT);
    EXPECT_EQ(geometry.getMeshMode(), MeshMode::MESH_EDIT);

    geometry.setMeshMode(MeshMode::CORNERS_ONLY);
    EXPECT_EQ(geometry.getMeshMode(), MeshMode::CORNERS_ONLY);
}

TEST(QuadGeometryTest, GenerateMeshPositionsReturnsCorrectCount) {
    QuadGeometry geometry;
    geometry.setMeshResolution(4);

    auto positions = geometry.generateMeshPositions();

    // (resolution + 1)^2 vertices
    EXPECT_EQ(positions.size(), 5 * 5);
}

TEST(QuadGeometryTest, GenerateMeshPositionsCornersMatch) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    // Set up unit square
    geometry.setCorners({
        Vec2(0.0f, 0.0f),
        Vec2(1.0f, 0.0f),
        Vec2(1.0f, 1.0f),
        Vec2(0.0f, 1.0f)
    });

    auto positions = geometry.generateMeshPositions();

    // Check corners (3x3 grid for resolution 2)
    // Top-left is index 0
    EXPECT_NEAR(positions[0].x, 0.0f, 0.001f);
    EXPECT_NEAR(positions[0].y, 0.0f, 0.001f);

    // Top-right is index 2
    EXPECT_NEAR(positions[2].x, 1.0f, 0.001f);
    EXPECT_NEAR(positions[2].y, 0.0f, 0.001f);

    // Bottom-left is index 6
    EXPECT_NEAR(positions[6].x, 0.0f, 0.001f);
    EXPECT_NEAR(positions[6].y, 1.0f, 0.001f);

    // Bottom-right is index 8
    EXPECT_NEAR(positions[8].x, 1.0f, 0.001f);
    EXPECT_NEAR(positions[8].y, 1.0f, 0.001f);
}

// =============================================================================
// Mesh Vertex Access Tests
// =============================================================================

TEST(QuadGeometryTest, GetVertexCountReturnsCorrectValue) {
    QuadGeometry geometry;

    geometry.setMeshResolution(2);
    EXPECT_EQ(geometry.getVertexCount(), 9);  // 3x3

    geometry.setMeshResolution(4);
    EXPECT_EQ(geometry.getVertexCount(), 25);  // 5x5

    geometry.setMeshResolution(16);
    EXPECT_EQ(geometry.getVertexCount(), 289);  // 17x17
}

TEST(QuadGeometryTest, SetMeshVertexByRowCol) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    Vec2 offset(0.1f, 0.2f);
    geometry.setMeshVertex(1, 1, offset);

    Vec2 retrieved = geometry.getMeshVertex(1, 1);
    EXPECT_FLOAT_EQ(retrieved.x, 0.1f);
    EXPECT_FLOAT_EQ(retrieved.y, 0.2f);
}

TEST(QuadGeometryTest, SetMeshVertexByIndex) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    Vec2 offset(0.3f, 0.4f);
    geometry.setMeshVertexByIndex(4, offset);  // Center of 3x3 grid

    Vec2 retrieved = geometry.getMeshVertexByIndex(4);
    EXPECT_FLOAT_EQ(retrieved.x, 0.3f);
    EXPECT_FLOAT_EQ(retrieved.y, 0.4f);
}

TEST(QuadGeometryTest, GetMeshVertexByIndexReturnsZeroForInvalidIndex) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    // Invalid indices should return zero
    Vec2 result = geometry.getMeshVertexByIndex(-1);
    EXPECT_FLOAT_EQ(result.x, 0.0f);
    EXPECT_FLOAT_EQ(result.y, 0.0f);

    result = geometry.getMeshVertexByIndex(100);
    EXPECT_FLOAT_EQ(result.x, 0.0f);
    EXPECT_FLOAT_EQ(result.y, 0.0f);
}

TEST(QuadGeometryTest, SetMeshVerticesReplacesAllOffsets) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    std::vector<Vec2> offsets(9, Vec2(0.5f, 0.5f));
    geometry.setMeshVertices(offsets);

    for (int i = 0; i < 9; ++i) {
        Vec2 v = geometry.getMeshVertexByIndex(i);
        EXPECT_FLOAT_EQ(v.x, 0.5f);
        EXPECT_FLOAT_EQ(v.y, 0.5f);
    }
}

TEST(QuadGeometryTest, SetMeshVerticesIgnoresWrongSize) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);  // Expects 9 vertices

    // Set some offsets first
    geometry.setMeshVertexByIndex(0, Vec2(0.1f, 0.1f));

    // Try to set wrong size - should be ignored
    std::vector<Vec2> wrongSize(5, Vec2(0.5f, 0.5f));
    geometry.setMeshVertices(wrongSize);

    // Original value should remain
    Vec2 v = geometry.getMeshVertexByIndex(0);
    EXPECT_FLOAT_EQ(v.x, 0.1f);
    EXPECT_FLOAT_EQ(v.y, 0.1f);
}

// =============================================================================
// Mesh Modification Detection Tests
// =============================================================================

TEST(QuadGeometryTest, HasMeshModificationsReturnsFalseInitially) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    EXPECT_FALSE(geometry.hasMeshModifications());
}

TEST(QuadGeometryTest, HasMeshModificationsReturnsTrueAfterModification) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    geometry.setMeshVertex(1, 1, Vec2(0.1f, 0.0f));

    EXPECT_TRUE(geometry.hasMeshModifications());
}

TEST(QuadGeometryTest, HasMeshModificationsReturnsFalseAfterReset) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    geometry.setMeshVertex(1, 1, Vec2(0.1f, 0.1f));
    EXPECT_TRUE(geometry.hasMeshModifications());

    geometry.resetMesh();
    EXPECT_FALSE(geometry.hasMeshModifications());
}

// =============================================================================
// Base Position Generation Tests
// =============================================================================

TEST(QuadGeometryTest, GenerateBasePositionsIgnoresMeshOffsets) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);
    geometry.setMeshMode(MeshMode::MESH_EDIT);

    // Set up unit square
    geometry.setCorners({
        Vec2(0.0f, 0.0f),
        Vec2(1.0f, 0.0f),
        Vec2(1.0f, 1.0f),
        Vec2(0.0f, 1.0f)
    });

    // Add an offset to the center vertex
    geometry.setMeshVertex(1, 1, Vec2(0.5f, 0.5f));

    // generateBasePositions should ignore offsets
    auto base = geometry.generateBasePositions();
    EXPECT_NEAR(base[4].x, 0.5f, 0.001f);
    EXPECT_NEAR(base[4].y, 0.5f, 0.001f);

    // generateMeshPositions should apply offsets in MESH_EDIT mode
    auto mesh = geometry.generateMeshPositions();
    EXPECT_NEAR(mesh[4].x, 1.0f, 0.001f);
    EXPECT_NEAR(mesh[4].y, 1.0f, 0.001f);
}

// =============================================================================
// Resolution Change Tests
// =============================================================================

TEST(QuadGeometryTest, ResolutionChangeResetsMeshVertices) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    // Set some offsets
    geometry.setMeshVertex(1, 1, Vec2(0.5f, 0.5f));
    EXPECT_TRUE(geometry.hasMeshModifications());

    // Change resolution
    geometry.setMeshResolution(4);

    // Mesh vertices should be reset
    EXPECT_FALSE(geometry.hasMeshModifications());
    EXPECT_EQ(geometry.getVertexCount(), 25);
}

// --- Non-uniform control grid (ADR-038, Tier 1) ----------------------------

TEST(QuadGeometryControlGridTest, DefaultGridIsUniform) {
    QuadGeometry g;
    EXPECT_TRUE(g.isUniform());
    EXPECT_EQ(g.getColumns(), DEFAULT_MESH_RESOLUTION);
    EXPECT_EQ(g.getRows(), DEFAULT_MESH_RESOLUTION);
    EXPECT_EQ(g.vertsPerRow(), DEFAULT_MESH_RESOLUTION + 1);
    // Endpoints pinned to 0 and 1.
    EXPECT_FLOAT_EQ(g.getUCoords().front(), 0.0f);
    EXPECT_FLOAT_EQ(g.getUCoords().back(), 1.0f);
}

TEST(QuadGeometryControlGridTest, SetUniformGridRebuildsEvenCoords) {
    QuadGeometry g;
    g.setUniformGrid(4);
    ASSERT_EQ(g.getColumns(), 4);
    ASSERT_EQ(g.getUCoords().size(), 5u);
    EXPECT_FLOAT_EQ(g.getUCoord(0), 0.0f);
    EXPECT_FLOAT_EQ(g.getUCoord(1), 0.25f);
    EXPECT_FLOAT_EQ(g.getUCoord(2), 0.5f);
    EXPECT_FLOAT_EQ(g.getUCoord(4), 1.0f);
    EXPECT_EQ(g.getVertexCount(), 25);
}

TEST(QuadGeometryControlGridTest, InsertUDivisionAddsLineAndGrowsGrid) {
    QuadGeometry g;
    g.setUniformGrid(2);                 // U coords: 0, 0.5, 1   (3 lines)
    EXPECT_EQ(g.getVertexCount(), 9);
    g.insertUDivision(0.25f);            // -> 0, 0.25, 0.5, 1    (4 lines)
    EXPECT_EQ(g.vertsPerRow(), 4);
    EXPECT_EQ(g.getVertexCount(), 12);
    EXPECT_FALSE(g.isUniform());
    EXPECT_FLOAT_EQ(g.getUCoord(1), 0.25f);
    // Coords stay sorted with pinned endpoints.
    EXPECT_FLOAT_EQ(g.getUCoords().front(), 0.0f);
    EXPECT_FLOAT_EQ(g.getUCoords().back(), 1.0f);
}

TEST(QuadGeometryControlGridTest, InsertedVerticesLandOnTheCurrentSurface) {
    QuadGeometry g;
    g.setUniformGrid(2);
    g.setMeshMode(MeshMode::MESH_EDIT);
    g.setCorners({Vec2(0.10f, 0.10f), Vec2(0.90f, 0.30f),
                  Vec2(0.90f, 0.70f), Vec2(0.10f, 0.90f)});
    g.setMeshVertex(0, 1, Vec2(0.2f, 0.0f));
    g.setMeshVertex(1, 1, Vec2(0.0f, 0.05f));
    // A new column: each of its vertices is where the surface already was.
    std::vector<Vec2> column;
    for (float v : g.getVCoords()) column.push_back(g.surfacePoint(0.25f, v));
    ASSERT_EQ(g.insertUDivision(0.25f), 1);
    EXPECT_NEAR(g.getMeshVertex(0, 2).x, 0.2f, 1e-6f);  // old col1 shifted right
    std::vector<Vec2> positions = g.generateMeshPositions();
    for (size_t r = 0; r < column.size(); ++r) {
        const Vec2 p = positions[r * 4 + 1];
        EXPECT_NEAR(p.x, column[r].x, 1e-5f) << "row " << r;
        EXPECT_NEAR(p.y, column[r].y, 1e-5f) << "row " << r;
    }

    // A new row, likewise.
    std::vector<Vec2> row;
    for (float u : g.getUCoords()) row.push_back(g.surfacePoint(u, 0.3f));
    ASSERT_EQ(g.insertVDivision(0.3f), 1);
    positions = g.generateMeshPositions();
    for (size_t c = 0; c < row.size(); ++c) {
        const Vec2 p = positions[4 + c];
        EXPECT_NEAR(p.x, row[c].x, 1e-5f) << "column " << c;
        EXPECT_NEAR(p.y, row[c].y, 1e-5f) << "column " << c;
    }
}

TEST(QuadGeometryControlGridTest, RemoveUDivisionDropsInteriorLinePreservingOthers) {
    QuadGeometry g;
    g.setUniformGrid(4);                 // 0,.25,.5,.75,1
    g.setMeshMode(MeshMode::MESH_EDIT);
    g.setMeshVertex(0, 3, Vec2(0.5f, 0.0f));  // mark the 0.75 column
    g.removeUDivision(2);                 // remove the 0.5 line
    EXPECT_EQ(g.getColumns(), 3);        // 0,.25,.75,1
    EXPECT_FLOAT_EQ(g.getUCoord(2), 0.75f);
    // The marked column (was col3) is now col2 and keeps its offset.
    EXPECT_NEAR(g.getMeshVertex(0, 2).x, 0.5f, 1e-4f);
}

TEST(QuadGeometryControlGridTest, EndpointsAndDuplicatesRejected) {
    QuadGeometry g;
    g.setUniformGrid(2);
    const int before = g.vertsPerRow();
    g.insertUDivision(0.0f);   // endpoint
    g.insertUDivision(1.0f);   // endpoint
    g.insertUDivision(0.5f);   // duplicate of existing line
    EXPECT_EQ(g.vertsPerRow(), before);
    g.removeUDivision(0);      // can't remove endpoint
    EXPECT_EQ(g.vertsPerRow(), before);
}

// --- ADR-038 stopgap: clean 1x1 start, insert-returns-index, drag-to-position ---

TEST(QuadGeometryControlGridTest, DefaultGeometryStartsAsSingleCell) {
    QuadGeometry g;
    EXPECT_EQ(g.getColumns(), 1);   // no internal divisions out of the box
    EXPECT_EQ(g.getRows(), 1);
    EXPECT_EQ(g.vertsPerRow(), 2);
}

TEST(QuadGeometryControlGridTest, InsertDivisionReturnsInsertedIndex) {
    QuadGeometry g;                 // 1x1: uCoords {0,1}
    const int col = g.insertUDivision(0.4f);
    EXPECT_EQ(col, 1);              // inserted between the two endpoints
    EXPECT_FLOAT_EQ(g.getUCoord(col), 0.4f);
    const int row = g.insertVDivision(0.6f);
    EXPECT_EQ(row, 1);
    EXPECT_FLOAT_EQ(g.getVCoord(row), 0.6f);
    // Rejected inserts report -1 and don't change the grid.
    EXPECT_EQ(g.insertUDivision(0.4f), -1);  // duplicate
    EXPECT_EQ(g.insertUDivision(1.0f), -1);  // endpoint
}

TEST(QuadGeometryControlGridTest, MoveDivisionRepositionsLineClampedBetweenNeighbours) {
    QuadGeometry g;
    g.setUniformGrid(4);                 // 0,.25,.5,.75,1
    g.moveUDivision(2, 0.40f);           // move the .5 line left
    EXPECT_FLOAT_EQ(g.getUCoord(2), 0.40f);
    // Cannot cross a neighbour: clamps to just inside [.25, .75].
    g.moveUDivision(2, 0.95f);
    EXPECT_LT(g.getUCoord(2), 0.75f);
    EXPECT_GT(g.getUCoord(2), 0.74f);
    // Endpoints never move.
    g.moveUDivision(0, 0.5f);
    EXPECT_FLOAT_EQ(g.getUCoord(0), 0.0f);
}

// --- Surface hit-test (ADR-049: click-to-select and click-on-edge insert) ------

TEST(QuadGeometryControlGridTest, SurfaceParamsAtRoundTripsOnKeystonedQuad) {
    QuadGeometry g;
    g.setCorners({Vec2(0.20f, 0.25f), Vec2(0.80f, 0.15f),
                  Vec2(0.90f, 0.85f), Vec2(0.10f, 0.75f)});
    // The projective base is drawn exactly by the tessellation, so (u,v) comes back.
    const float samples[] = {0.1f, 0.25f, 0.5f, 0.7f, 0.9f};
    for (float u : samples) {
        for (float v : samples) {
            const Vec2 uv = g.surfaceParamsAt(g.surfacePoint(u, v));
            EXPECT_NEAR(uv.x, u, 1e-4f) << "u=" << u << " v=" << v;
            EXPECT_NEAR(uv.y, v, 1e-4f) << "u=" << u << " v=" << v;
        }
    }
    EXPECT_LT(g.surfaceParamsAt(Vec2(0.05f, 0.5f)).x, 0.0f);  // left of the quad
}

TEST(QuadGeometryControlGridTest, SurfaceParamsAtFollowsTheMeshWarp) {
    QuadGeometry g;
    g.setCorners({Vec2(0.2f, 0.2f), Vec2(0.8f, 0.2f), Vec2(0.8f, 0.8f), Vec2(0.2f, 0.8f)});
    g.setUniformGrid(2);
    g.setMeshMode(MeshMode::MESH_EDIT);
    g.setMeshVertex(1, 2, Vec2(0.15f, 0.0f));  // pull the right edge's middle outwards
    const Vec2 bulge(0.9f, 0.5f);               // outside the corners, inside the warp
    const Vec2 uv = g.surfaceParamsAt(bulge);
    ASSERT_GE(uv.x, 0.0f);
    EXPECT_GT(uv.x, 0.9f);  // near the pulled edge, where the base is at x < 0.8
    const Vec2 p = g.surfacePoint(uv.x, uv.y);  // within the tessellation's chord error
    EXPECT_NEAR(p.x, bulge.x, 2e-3f);
    EXPECT_NEAR(p.y, bulge.y, 2e-3f);

    g.setMeshMode(MeshMode::CORNERS_ONLY);  // the warp is not drawn in corner mode
    EXPECT_LT(g.surfaceParamsAt(bulge).x, 0.0f);
}

// --- Perspective-correct surface (foundation audit 0.1) ------------------------

namespace {

// A strongly keystoned trapezoid, as a projector hitting a wall at an angle.
QuadGeometry keystonedQuad() {
    QuadGeometry g;
    g.setCorners({Vec2(0.10f, 0.10f), Vec2(0.90f, 0.30f),
                  Vec2(0.90f, 0.70f), Vec2(0.10f, 0.90f)});
    return g;
}

// n + 1 even parameters 0, 1/n, ..., 1.
std::vector<float> evenParams(int n) {
    std::vector<float> params;
    for (int i = 0; i <= n; ++i) params.push_back(static_cast<float>(i) / n);
    return params;
}

float cross2(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }

// The classic bilinearly-blended Coons patch that surfacePoint() computed before
// the projective base, rebuilt from the public bezier controls.
Vec2 legacyCoons(const QuadGeometry& g, float u, float v) {
    const auto& c = g.getCorners();
    auto edge = [&](int e, float t) {
        const Vec2 p0 = c[e], p3 = c[(e + 1) % 4];
        const Vec2 c1 = g.getBezierControl(e, 0), c2 = g.getBezierControl(e, 1);
        const float mt = 1.0f - t;
        return mt * mt * mt * p0 + 3.0f * mt * mt * t * c1 + 3.0f * mt * t * t * c2
             + t * t * t * p3;
    };
    const Vec2 ruledV = (1.0f - v) * edge(0, u) + v * edge(2, 1.0f - u);
    const Vec2 ruledU = (1.0f - u) * edge(3, 1.0f - v) + u * edge(1, v);
    return ruledV + ruledU - g.transformUVToPosition(Vec2(u, v));
}

}  // namespace

TEST(QuadGeometrySurfaceTest, KeystoneCentreLandsOnDiagonalIntersection) {
    // Under perspective the centre of a flat rectangle projects to where the quad's
    // diagonals cross; the old bilinear patch put it at the corner average instead.
    const QuadGeometry g = keystonedQuad();
    const auto& c = g.getCorners();
    const Vec2 d1 = c[2] - c[0];
    const Vec2 d2 = c[3] - c[1];
    const float t = cross2(c[1] - c[0], d2) / cross2(d1, d2);
    const Vec2 intersection = c[0] + t * d1;

    const Vec2 centre = g.surfacePoint(0.5f, 0.5f);
    EXPECT_NEAR(centre.x, intersection.x, 1e-5f);
    EXPECT_NEAR(centre.y, intersection.y, 1e-5f);
}

TEST(QuadGeometrySurfaceTest, KeystoneMapsStraightLinesToStraightLines) {
    // Projective maps keep every straight line straight, including the diagonal
    // u == v, which a bilinear patch bends into a curve.
    const QuadGeometry g = keystonedQuad();
    const Vec2 a = g.surfacePoint(0.0f, 0.0f);
    const Vec2 b = g.surfacePoint(1.0f, 1.0f);
    for (float s = 0.1f; s < 1.0f; s += 0.1f) {
        const Vec2 p = g.surfacePoint(s, s);
        EXPECT_NEAR(cross2(b - a, p - a), 0.0f, 1e-5f) << "s=" << s;
    }
}

TEST(QuadGeometrySurfaceTest, CornersMapExactly) {
    const QuadGeometry g = keystonedQuad();
    const auto& c = g.getCorners();
    const Vec2 uv[4] = {Vec2(0, 0), Vec2(1, 0), Vec2(1, 1), Vec2(0, 1)};
    for (int i = 0; i < 4; ++i) {
        const Vec2 p = g.surfacePoint(uv[i].x, uv[i].y);
        EXPECT_NEAR(p.x, c[i].x, 1e-6f) << "corner " << i;
        EXPECT_NEAR(p.y, c[i].y, 1e-6f) << "corner " << i;
    }
}

TEST(QuadGeometrySurfaceTest, ParallelogramMatchesBilinear) {
    // For an affine quad the homography is affine, so nothing changes for
    // unwarped or merely sheared quads.
    QuadGeometry g;
    g.setCorners({Vec2(0.2f, 0.1f), Vec2(0.7f, 0.2f),
                  Vec2(0.8f, 0.6f), Vec2(0.3f, 0.5f)});
    for (float u = 0.0f; u <= 1.0f; u += 0.25f) {
        for (float v = 0.0f; v <= 1.0f; v += 0.25f) {
            const Vec2 p = g.surfacePoint(u, v);
            const Vec2 q = g.transformUVToPosition(Vec2(u, v));
            EXPECT_NEAR(p.x, q.x, 1e-5f);
            EXPECT_NEAR(p.y, q.y, 1e-5f);
        }
    }
}

TEST(QuadGeometrySurfaceTest, ConcaveQuadFallsBackToBilinear) {
    // No projective map exists for a concave quad; keep the old bilinear surface
    // rather than producing a fold or a pole.
    QuadGeometry g;
    g.setCorners({Vec2(0.1f, 0.1f), Vec2(0.9f, 0.1f),
                  Vec2(0.3f, 0.3f), Vec2(0.1f, 0.9f)});
    for (float u = 0.0f; u <= 1.0f; u += 0.25f) {
        for (float v = 0.0f; v <= 1.0f; v += 0.25f) {
            const Vec2 p = g.surfacePoint(u, v);
            const Vec2 q = g.transformUVToPosition(Vec2(u, v));
            ASSERT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
            EXPECT_NEAR(p.x, q.x, 1e-6f);
            EXPECT_NEAR(p.y, q.y, 1e-6f);
        }
    }
}

TEST(QuadGeometrySurfaceTest, BezierOnParallelogramIsUnchanged) {
    // Existing curved-edge projects on unkeystoned quads must render identically.
    QuadGeometry g;
    g.setBezierEnabled(true);
    g.setBezierControl(0, 0, Vec2(0.40f, 0.20f));
    g.setBezierControl(1, 1, Vec2(0.78f, 0.62f));
    g.setBezierControl(3, 0, Vec2(0.25f, 0.55f));
    for (float u = 0.0f; u <= 1.0f; u += 0.125f) {
        for (float v = 0.0f; v <= 1.0f; v += 0.125f) {
            const Vec2 p = g.surfacePoint(u, v);
            const Vec2 q = legacyCoons(g, u, v);
            EXPECT_NEAR(p.x, q.x, 1e-5f) << "u=" << u << " v=" << v;
            EXPECT_NEAR(p.y, q.y, 1e-5f) << "u=" << u << " v=" << v;
        }
    }
}

namespace {

// Largest distance from any surface point along edge `edge` to that edge's bezier
// curve, for boundaries sampled at 11 parameters.
float maxBoundaryDistanceToCurve(const QuadGeometry& g, int edge) {
    const auto& c = g.getCorners();
    const Vec2 p0 = c[edge], p3 = c[(edge + 1) % 4];
    const Vec2 c1 = g.getBezierControl(edge, 0), c2 = g.getBezierControl(edge, 1);
    std::vector<Vec2> curve;
    for (int i = 0; i <= 4000; ++i) {
        const float t = i / 4000.0f, mt = 1.0f - t;
        curve.push_back(mt * mt * mt * p0 + 3.0f * mt * mt * t * c1
                        + 3.0f * mt * t * t * c2 + t * t * t * p3);
    }
    float worst = 0.0f;
    for (int i = 0; i <= 10; ++i) {
        const float t = i / 10.0f;
        const Vec2 uv[4] = {Vec2(t, 0.0f), Vec2(1.0f, t), Vec2(t, 1.0f), Vec2(0.0f, t)};
        const Vec2 p = g.surfacePoint(uv[edge].x, uv[edge].y);
        float best = 1.0f;
        for (const Vec2& q : curve) best = std::min(best, glm::length(p - q));
        worst = std::max(worst, best);
    }
    return worst;
}

// Bows every edge outward by a different amount.
void curveAllEdges(QuadGeometry& g) {
    g.setBezierEnabled(true);
    const Vec2 bows[4] = {Vec2(0.0f, -0.05f), Vec2(0.04f, 0.0f),
                          Vec2(0.0f, 0.06f), Vec2(-0.03f, 0.0f)};
    for (int e = 0; e < 4; ++e) {
        g.setBezierControl(e, 0, g.getBezierControl(e, 0) + bows[e]);
        g.setBezierControl(e, 1, g.getBezierControl(e, 1) + 0.5f * bows[e]);
    }
}

}  // namespace

TEST(QuadGeometrySurfaceTest, BezierBoundariesStayOnEdgeCurvesWhenKeystoned) {
    // Over the projective base, each boundary must still trace its bezier curve
    // exactly (only the parameterisation along it changes). Covers the reversed
    // edges 2 and 3 as well as 0 and 1.
    QuadGeometry g = keystonedQuad();
    curveAllEdges(g);
    for (int e = 0; e < 4; ++e) {
        EXPECT_LT(maxBoundaryDistanceToCurve(g, e), 5e-4f) << "edge " << e;
    }
}

TEST(QuadGeometrySurfaceTest, MirroredKeystoneIsProjectiveAndKeepsBezierBoundaries) {
    // Dragging corners past each other gives a clockwise (mirrored) quad; it is
    // still convex, so it gets the projective base too.
    QuadGeometry g;
    g.setCorners({Vec2(0.90f, 0.30f), Vec2(0.10f, 0.10f),
                  Vec2(0.10f, 0.90f), Vec2(0.90f, 0.70f)});
    const auto& c = g.getCorners();
    const Vec2 d1 = c[2] - c[0];
    const Vec2 d2 = c[3] - c[1];
    const Vec2 intersection = c[0] + (cross2(c[1] - c[0], d2) / cross2(d1, d2)) * d1;
    const Vec2 centre = g.surfacePoint(0.5f, 0.5f);
    EXPECT_NEAR(centre.x, intersection.x, 1e-5f);
    EXPECT_NEAR(centre.y, intersection.y, 1e-5f);

    curveAllEdges(g);
    for (int e = 0; e < 4; ++e) {
        EXPECT_LT(maxBoundaryDistanceToCurve(g, e), 5e-4f) << "edge " << e;
    }
}

TEST(QuadGeometrySurfaceTest, SampledTexCoordInterpolationIsExactlyProjective) {
    // The renderer interpolates (u*q, v*q, q) linearly across a triangle and divides
    // by q. For the projective base that must recover the true (u,v) at any point
    // between samples, however coarse the tessellation or strong the keystone.
    QuadGeometry g;
    g.setCorners({Vec2(0.10f, 0.10f), Vec2(0.90f, 0.40f),
                  Vec2(0.90f, 0.60f), Vec2(0.10f, 0.90f)});  // 4:1 keystone
    const int n = 2;
    const auto samples = g.sampleSurface(evenParams(n), evenParams(n));
    ASSERT_EQ(samples.size(), 9u);

    // Midpoint of the edge between sample (0,0) and sample (1,1) of the coarse grid.
    const auto& a = samples[0];
    const auto& b = samples[4];
    const Vec2 screenMid = 0.5f * (a.position + b.position);
    const float q = 0.5f * (a.q + b.q);
    const Vec2 uv = 0.5f * (Vec2(0.0f, 0.0f) * a.q + Vec2(0.5f, 0.5f) * b.q) / q;

    const Vec2 p = g.surfacePoint(uv.x, uv.y);
    EXPECT_NEAR(p.x, screenMid.x, 1e-5f);
    EXPECT_NEAR(p.y, screenMid.y, 1e-5f);
}

TEST(QuadGeometrySurfaceTest, SampleSurfaceMatchesSurfacePoint) {
    QuadGeometry g = keystonedQuad();
    curveAllEdges(g);
    const int n = 8;
    const auto samples = g.sampleSurface(evenParams(n), evenParams(n));
    ASSERT_EQ(samples.size(), static_cast<size_t>((n + 1) * (n + 1)));
    for (int r = 0; r <= n; ++r) {
        for (int c = 0; c <= n; ++c) {
            const Vec2 p = g.surfacePoint(static_cast<float>(c) / n, static_cast<float>(r) / n);
            const Vec2& s = samples[static_cast<size_t>(r * (n + 1) + c)].position;
            EXPECT_NEAR(s.x, p.x, 1e-6f);
            EXPECT_NEAR(s.y, p.y, 1e-6f);
        }
    }
}

// --- Unified surface evaluator S = B + R (ADR-049) ------------------------------

namespace {

// A keystoned, bezier-edged quad in mesh edit on a non-uniform 3x2 grid, with every
// control vertex moved by a different residual.
QuadGeometry warpedMesh() {
    QuadGeometry g = keystonedQuad();
    curveAllEdges(g);
    g.setGridCoords({0.0f, 0.3f, 0.55f, 1.0f}, {0.0f, 0.4f, 1.0f});
    g.setMeshMode(MeshMode::MESH_EDIT);
    for (int i = 0; i < g.getVertexCount(); ++i) {
        g.setMeshVertexByIndex(i, Vec2(0.02f * std::sin(1.7f * i + 0.3f),
                                       0.03f * std::cos(2.3f * i + 0.1f)));
    }
    return g;
}

// One-sided dS/du (dS/dv when !alongU) at (u,v), second-order accurate.
Vec2 sideDerivative(const QuadGeometry& g, float u, float v, bool alongU, bool after) {
    constexpr float H = 2e-3f;
    const float s = after ? H : -H;
    auto at = [&](float k) {
        return alongU ? g.surfacePoint(u + k * s, v) : g.surfacePoint(u, v + k * s);
    };
    return (-3.0f * at(0.0f) + 4.0f * at(1.0f) - at(2.0f)) / (2.0f * s);
}

}  // namespace

TEST(QuadGeometryEvaluatorTest, ZeroResidualsAreTheCornerSurfaceExactly) {
    // Corner-only equivalence: entering mesh edit with nothing moved changes nothing.
    QuadGeometry corner = keystonedQuad();
    curveAllEdges(corner);
    corner.setGridCoords({0.0f, 0.3f, 0.55f, 1.0f}, {0.0f, 0.4f, 1.0f});
    QuadGeometry mesh = corner;
    mesh.setMeshMode(MeshMode::MESH_EDIT);
    EXPECT_FALSE(mesh.hasResidualSurface());
    EXPECT_EQ(mesh.tessellationU(), corner.tessellationU());
    const auto a = corner.sampleSurface(corner.tessellationU(), corner.tessellationV());
    const auto b = mesh.sampleSurface(mesh.tessellationU(), mesh.tessellationV());
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].position, b[i].position) << i;
        EXPECT_EQ(a[i].q, b[i].q) << i;
    }
    // And in corner mode a warp is not drawn at all.
    QuadGeometry hidden = warpedMesh();
    hidden.setMeshMode(MeshMode::CORNERS_ONLY);
    EXPECT_EQ(hidden.surfacePoint(0.4f, 0.7f), hidden.basePoint(0.4f, 0.7f));
}

TEST(QuadGeometryEvaluatorTest, ExactAtControlVertices) {
    const QuadGeometry g = warpedMesh();
    const auto positions = g.generateMeshPositions();
    const auto samples = g.sampleSurface(g.getUCoords(), g.getVCoords());
    for (int r = 0; r < g.vertsPerCol(); ++r) {
        for (int c = 0; c < g.vertsPerRow(); ++c) {
            const size_t i = static_cast<size_t>(r * g.vertsPerRow() + c);
            const Vec2 expected = g.basePoint(g.getUCoord(c), g.getVCoord(r)) + g.getMeshVertices()[i];
            EXPECT_NEAR(positions[i].x, expected.x, 1e-6f);
            EXPECT_NEAR(positions[i].y, expected.y, 1e-6f);
            EXPECT_NEAR(samples[i].position.x, expected.x, 1e-6f);
            EXPECT_NEAR(samples[i].position.y, expected.y, 1e-6f);
        }
    }
}

TEST(QuadGeometryEvaluatorTest, C1AcrossCellBoundariesWithAutoTangents) {
    const QuadGeometry g = warpedMesh();
    // Across each interior U line and V line, at vertices and mid-edge.
    for (int c = 1; c + 1 < g.vertsPerRow(); ++c) {
        for (float v : {0.2f, 0.4f, 0.75f}) {
            const Vec2 l = sideDerivative(g, g.getUCoord(c), v, true, false);
            const Vec2 r = sideDerivative(g, g.getUCoord(c), v, true, true);
            EXPECT_LT(glm::length(l - r), 2e-3f) << "u line " << c << " v " << v;
        }
    }
    for (float u : {0.1f, 0.3f, 0.42f, 0.8f}) {
        const Vec2 above = sideDerivative(g, u, g.getVCoord(1), false, false);
        const Vec2 below = sideDerivative(g, u, g.getVCoord(1), false, true);
        EXPECT_LT(glm::length(above - below), 2e-3f) << "v line at u " << u;
    }
}

TEST(QuadGeometryEvaluatorTest, ExplicitTangentsAreHonoured) {
    QuadGeometry g = warpedMesh();
    const int vertex = 1 * g.vertsPerRow() + 1;  // interior: u = 0.3, v = 0.4
    const float u = g.getUCoord(1), v = g.getVCoord(1);
    const Vec2 leftBefore = sideDerivative(g, u, v, true, false);
    EXPECT_EQ(g.availableHandles(vertex), 0xFu);

    const Vec2 target = g.gridHandlePosition(vertex, GRID_HANDLE_U_AFTER) + Vec2(0.02f, -0.03f);
    g.setGridHandlePosition(vertex, GRID_HANDLE_U_AFTER, target);
    EXPECT_TRUE(g.getVertexTangent(vertex).isExplicit(GRID_HANDLE_U_AFTER));
    const Vec2 back = g.gridHandlePosition(vertex, GRID_HANDLE_U_AFTER);
    EXPECT_NEAR(back.x, target.x, 1e-5f);
    EXPECT_NEAR(back.y, target.y, 1e-5f);

    // The curve leaves the vertex towards the handle (bezier convention: the handle
    // sits a third of the cell along the tangent) ...
    const Vec2 vertexPos = g.generateMeshPositions()[static_cast<size_t>(vertex)];
    const float span = g.getUCoord(2) - u;
    const Vec2 expected = 3.0f * (target - vertexPos) / span;
    const Vec2 right = sideDerivative(g, u, v, true, true);
    EXPECT_LT(glm::length(right - expected), 3e-3f * glm::length(expected));
    // ... and the other side keeps its automatic tangent, so the line now kinks.
    EXPECT_LT(glm::length(sideDerivative(g, u, v, true, false) - leftBefore), 2e-3f);
    EXPECT_GT(glm::length(right - leftBefore), 0.05f);

    // A V handle moves the column line the same way.
    const Vec2 vTarget = g.gridHandlePosition(vertex, GRID_HANDLE_V_BEFORE) + Vec2(-0.01f, 0.0f);
    g.setGridHandlePosition(vertex, GRID_HANDLE_V_BEFORE, vTarget);
    const Vec2 up = sideDerivative(g, u, v, false, false);
    const Vec2 vExpected = -3.0f * (vTarget - vertexPos) / (v - g.getVCoord(0));
    EXPECT_LT(glm::length(up - vExpected), 3e-3f * glm::length(vExpected));
}

TEST(QuadGeometryEvaluatorTest, BoundaryVerticesHaveNoOutwardHandles) {
    QuadGeometry g = warpedMesh();
    EXPECT_EQ(g.availableHandles(0),  // top-left corner
              (1u << GRID_HANDLE_U_AFTER) | (1u << GRID_HANDLE_V_AFTER));
    const int last = g.getVertexCount() - 1;  // bottom-right corner
    EXPECT_EQ(g.availableHandles(last),
              (1u << GRID_HANDLE_U_BEFORE) | (1u << GRID_HANDLE_V_BEFORE));
    const VertexTangents before = g.getVertexTangent(0);
    g.setGridHandlePosition(0, GRID_HANDLE_U_BEFORE, Vec2(0.0f));  // does not exist
    EXPECT_EQ(g.getVertexTangent(0), before);
}

TEST(QuadGeometryEvaluatorTest, HandlesAndVerticesFollowTheCorners) {
    // Residuals and tangents are relative to B, so moving the corners carries the
    // mesh with them: a translation moves every vertex and handle by the same amount.
    QuadGeometry g = warpedMesh();
    const int vertex = 1 * g.vertsPerRow() + 2;
    g.setGridHandlePosition(vertex, GRID_HANDLE_V_AFTER,
                            g.gridHandlePosition(vertex, GRID_HANDLE_V_AFTER) + Vec2(0.03f, 0.0f));
    const Vec2 handle = g.gridHandlePosition(vertex, GRID_HANDLE_V_AFTER);
    const Vec2 position = g.generateMeshPositions()[static_cast<size_t>(vertex)];
    const Vec2 d(0.05f, -0.02f);
    std::array<Vec2, 4> corners = g.getCorners();
    for (Vec2& c : corners) c += d;
    g.setCorners(corners);
    const Vec2 movedHandle = g.gridHandlePosition(vertex, GRID_HANDLE_V_AFTER);
    const Vec2 movedPosition = g.generateMeshPositions()[static_cast<size_t>(vertex)];
    EXPECT_NEAR(movedHandle.x, handle.x + d.x, 1e-5f);
    EXPECT_NEAR(movedHandle.y, handle.y + d.y, 1e-5f);
    EXPECT_NEAR(movedPosition.x, position.x + d.x, 1e-5f);
    EXPECT_NEAR(movedPosition.y, position.y + d.y, 1e-5f);
}

TEST(QuadGeometryEvaluatorTest, BilinearOffsetsConvertToTheSameAbsolutePositions) {
    // A pre-ADR-049 file: offsets from the bilinear patch on a keystoned, curved quad.
    QuadGeometry g = warpedMesh();
    std::vector<Vec2> expected;
    for (int r = 0; r < g.vertsPerCol(); ++r) {
        for (int c = 0; c < g.vertsPerRow(); ++c) {
            expected.push_back(g.transformUVToPosition(Vec2(g.getUCoord(c), g.getVCoord(r))) +
                               g.getMeshVertex(r, c));
        }
    }
    g.convertBilinearOffsetsToResiduals();
    const auto positions = g.generateMeshPositions();
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(positions[i].x, expected[i].x, 1e-5f) << i;
        EXPECT_NEAR(positions[i].y, expected[i].y, 1e-5f) << i;
    }
}

TEST(QuadGeometryEvaluatorTest, ResetMeshClearsResidualsAndHandles) {
    QuadGeometry g = warpedMesh();
    g.setGridHandlePosition(5, GRID_HANDLE_U_BEFORE, Vec2(0.5f, 0.5f));  // row 1, column 1
    ASSERT_TRUE(g.hasExplicitTangents());
    g.resetMesh();
    EXPECT_FALSE(g.hasMeshModifications());
    EXPECT_FALSE(g.hasResidualSurface());
    EXPECT_EQ(g.surfacePoint(0.3f, 0.6f), g.basePoint(0.3f, 0.6f));
}

// --- Warp mode (ADR-058) ---------------------------------------------------------

// Two quads tilted opposite ways that share the edge A.TR-A.BR = B.TL-B.BL, as the
// owner snapped them (2026-11-23).
std::array<QuadGeometry, 2> foldedPair(WarpMode mode) {
    QuadGeometry a, b;
    a.setCorners({Vec2(0.10f, 0.25f), Vec2(0.62f, 0.30f), Vec2(0.60f, 0.70f), Vec2(0.11f, 0.85f)});
    b.setCorners({Vec2(0.62f, 0.30f), Vec2(0.90f, 0.15f), Vec2(0.95f, 0.80f), Vec2(0.60f, 0.70f)});
    a.setWarpMode(mode);
    b.setWarpMode(mode);
    return {a, b};
}

TEST(QuadGeometryWarpModeTest, DefaultIsPerspective) {
    EXPECT_EQ(QuadGeometry().getWarpMode(), WarpMode::PERSPECTIVE);
}

TEST(QuadGeometryWarpModeTest, LinearQuadsAgreeAlongTheirSharedEdge) {
    const auto linear = foldedPair(WarpMode::LINEAR);
    for (int i = 0; i <= 8; ++i) {
        const float t = i / 8.0f;
        const Vec2 a = linear[0].basePoint(1.0f, t), b = linear[1].basePoint(0.0f, t);
        EXPECT_NEAR(a.x, b.x, 1e-6f) << t;
        EXPECT_NEAR(a.y, b.y, 1e-6f) << t;
    }
    // The bug this fixes: in perspective the same row lands at different places.
    const auto perspective = foldedPair(WarpMode::PERSPECTIVE);
    EXPECT_GT(glm::length(perspective[0].basePoint(1.0f, 0.5f) - perspective[1].basePoint(0.0f, 0.5f)),
              0.01f);
}

TEST(QuadGeometryWarpModeTest, LinearSamplesHaveNoProjectiveWeight) {
    QuadGeometry g = keystonedQuad();
    g.setWarpMode(WarpMode::LINEAR);
    for (const auto& s : g.sampleSurface({0.0f, 0.4f, 1.0f}, {0.0f, 0.7f, 1.0f})) {
        EXPECT_EQ(s.q, 1.0f);
    }
    EXPECT_EQ(g.basePoint(0.3f, 0.6f), g.transformUVToPosition(Vec2(0.3f, 0.6f)));
}

TEST(QuadGeometryWarpModeTest, SwitchingKeepsEveryControlVertex) {
    QuadGeometry g = warpedMesh();
    const auto before = g.generateMeshPositions();
    for (WarpMode mode : {WarpMode::LINEAR, WarpMode::PERSPECTIVE}) {
        g.setWarpMode(mode);
        EXPECT_EQ(g.getWarpMode(), mode);
        const auto after = g.generateMeshPositions();
        ASSERT_EQ(after.size(), before.size());
        for (size_t i = 0; i < before.size(); ++i) {
            EXPECT_NEAR(after[i].x, before[i].x, 1e-5f) << i;
            EXPECT_NEAR(after[i].y, before[i].y, 1e-5f) << i;
        }
    }
}

// ---- Linear / Spline points, Crop, edge and grid-line hits (owner, 2026-11-23) ----

namespace {

// The unit square, Linear warp, an even 2x2 grid in mesh edit with the centre point
// moved right by 0.1.
QuadGeometry bentSquare() {
    QuadGeometry g;
    g.setCorners({Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)});
    g.setWarpMode(WarpMode::LINEAR);
    g.setUniformGrid(2);
    g.setMeshMode(MeshMode::MESH_EDIT);
    g.setMeshVertex(1, 1, Vec2(0.1f, 0.0f));
    return g;
}

}  // namespace

TEST(GridPointTypeTest, LinearPointsRunTheGridStraightAndCellsBilinear) {
    QuadGeometry spline = bentSquare();
    QuadGeometry linear = bentSquare();
    linear.setAllVerticesLinear(true);
    EXPECT_TRUE(linear.hasLinearVertices());
    EXPECT_FALSE(spline.hasLinearVertices());

    // Along the middle row, halfway from the left edge (0, 0.5) to the moved centre
    // (0.6, 0.5): Linear lands on the chord, Spline bows away from it.
    EXPECT_NEAR(linear.surfacePoint(0.25f, 0.5f).x, 0.3f, 1e-5f);
    EXPECT_GT(std::fabs(spline.surfacePoint(0.25f, 0.5f).x - 0.3f), 1e-3f);

    // A cell with four Linear corners is the bilinear patch of its corners.
    const Vec2 tl(0.0f, 0.0f), tr(0.5f, 0.0f), bl(0.0f, 0.5f), br(0.6f, 0.5f);
    const Vec2 bilinear = glm::mix(glm::mix(tl, tr, 0.5f), glm::mix(bl, br, 0.5f), 0.5f);
    const Vec2 p = linear.surfacePoint(0.25f, 0.25f);
    EXPECT_NEAR(p.x, bilinear.x, 1e-5f);
    EXPECT_NEAR(p.y, bilinear.y, 1e-5f);

    // The points themselves stay put whatever their type.
    const Vec2 centre = linear.surfacePoint(0.5f, 0.5f);
    EXPECT_NEAR(centre.x, 0.6f, 1e-5f);
    EXPECT_NEAR(centre.y, 0.5f, 1e-5f);
}

TEST(GridPointTypeTest, HandlesAndLinearExcludeEachOther) {
    QuadGeometry g = bentSquare();
    const int centre = 4;  // row 1, col 1 of a 3x3 vertex grid
    g.setGridHandlePosition(centre, GRID_HANDLE_U_AFTER, Vec2(0.8f, 0.6f));
    ASSERT_TRUE(g.getVertexTangent(centre).isExplicit(GRID_HANDLE_U_AFTER));

    g.setVertexLinear(centre, true);
    EXPECT_TRUE(g.isVertexLinear(centre));
    EXPECT_EQ(g.getVertexTangent(centre).explicitMask, 0u) << "a Linear point has no handles";

    g.setGridHandlePosition(centre, GRID_HANDLE_V_AFTER, Vec2(0.6f, 0.8f));
    EXPECT_FALSE(g.isVertexLinear(centre)) << "dragging a handle makes it a Spline again";
}

TEST(GridPointTypeTest, NewLinesTakeTheChosenTypeAndResetMeshKeepsIt) {
    QuadGeometry g = bentSquare();
    const int col = g.insertUDivision(0.25f, /*linear=*/true);
    ASSERT_EQ(col, 1);
    for (int row = 0; row < g.vertsPerCol(); ++row) {
        EXPECT_TRUE(g.isVertexLinear(row * g.vertsPerRow() + col)) << row;
        EXPECT_FALSE(g.isVertexLinear(row * g.vertsPerRow() + 0)) << "the old lines keep theirs";
    }
    const int row = g.insertVDivision(0.75f);  // Spline, the default
    for (int c = 0; c < g.vertsPerRow(); ++c) {
        EXPECT_FALSE(g.isVertexLinear(row * g.vertsPerRow() + c)) << c;
    }

    g.resetMesh();
    EXPECT_FALSE(g.hasMeshModifications());
    EXPECT_TRUE(g.isVertexLinear(1)) << "Reset Mesh moves points back, not their type";
}

TEST(ContentModeTest, CropKeepsTheImageOnTheCornerPin) {
    QuadGeometry g = bentSquare();
    const std::vector<float> us{0.0f, 0.5f, 1.0f};
    const std::vector<float> vs{0.0f, 0.5f, 1.0f};
    const auto samples = g.sampleSurface(us, vs);

    // Warp: the moved centre samples the image's centre.
    EXPECT_FALSE(g.cropsImage());
    Vec3 c = g.imageCoords(us, vs, samples)[4];
    EXPECT_NEAR(c.x / c.z, 0.5f, 1e-5f);
    EXPECT_NEAR(c.y / c.z, 0.5f, 1e-5f);

    // Crop: it samples the image where it now sits, (0.6, 0.5).
    g.setContentMode(ContentMode::CROP);
    EXPECT_TRUE(g.cropsImage());
    c = g.imageCoords(us, vs, samples)[4];
    EXPECT_NEAR(c.x / c.z, 0.6f, 1e-4f);
    EXPECT_NEAR(c.y / c.z, 0.5f, 1e-4f);

    // With nothing bent, Crop draws as Warp.
    g.resetMesh();
    EXPECT_FALSE(g.cropsImage());
}

TEST(ContentModeTest, CropOnAPerspectiveQuadFindsTheProjectivePoint) {
    QuadGeometry g;
    g.setCorners({Vec2(0.1f, 0.1f), Vec2(0.9f, 0.2f), Vec2(0.8f, 0.9f), Vec2(0.2f, 0.8f)});
    g.setWarpMode(WarpMode::PERSPECTIVE);
    g.setUniformGrid(2);
    g.setMeshMode(MeshMode::MESH_EDIT);
    g.setMeshVertex(1, 1, Vec2(0.05f, -0.03f));
    g.setContentMode(ContentMode::CROP);
    const std::vector<float> us{0.5f}, vs{0.5f};
    const auto samples = g.sampleSurface(us, vs);
    const Vec3 c = g.imageCoords(us, vs, samples)[0];
    // The corner pin at the found (s, t) is the moved point.
    QuadGeometry plane = g;
    plane.setMeshMode(MeshMode::CORNERS_ONLY);
    const Vec2 back = plane.basePoint(c.x / c.z, c.y / c.z);
    EXPECT_NEAR(back.x, samples[0].position.x, 1e-4f);
    EXPECT_NEAR(back.y, samples[0].position.y, 1e-4f);
}

TEST(GridHitTest, ThePressNearTheEdgeAndNearAGridLine) {
    QuadGeometry g;
    g.setCorners({Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)});
    g.setWarpMode(WarpMode::LINEAR);
    g.setUniformGrid(2);
    g.setMeshMode(MeshMode::MESH_EDIT);

    const Vec2 top = g.boundaryParamsNear(Vec2(0.3f, -0.01f), 0.015f);
    EXPECT_NEAR(top.x, 0.3f, 1e-4f);
    EXPECT_NEAR(top.y, 0.0f, 1e-4f);
    const Vec2 left = g.boundaryParamsNear(Vec2(0.005f, 0.7f), 0.015f);
    EXPECT_NEAR(left.x, 0.0f, 1e-4f);
    EXPECT_NEAR(left.y, 0.7f, 1e-4f);
    EXPECT_LT(g.boundaryParamsNear(Vec2(0.5f, -0.2f), 0.015f).x, 0.0f) << "too far";

    bool column = false;
    EXPECT_EQ(g.gridLineNear(Vec2(0.505f, 0.3f), 0.008f, column), 1);
    EXPECT_TRUE(column);
    EXPECT_EQ(g.gridLineNear(Vec2(0.2f, 0.497f), 0.008f, column), 1);
    EXPECT_FALSE(column);
    EXPECT_EQ(g.gridLineNear(Vec2(0.25f, 0.25f), 0.008f, column), -1);
    EXPECT_EQ(g.gridLineNear(Vec2(0.0f, 0.3f), 0.008f, column), -1) << "the outline is not a line to move";

    // A bent line is found where it is drawn.
    g.setMeshVertex(1, 1, Vec2(0.1f, 0.0f));
    EXPECT_EQ(g.gridLineNear(Vec2(0.6f, 0.5f + 0.02f), 0.008f, column), 1);
    EXPECT_TRUE(column);
}

} // namespace test
} // namespace quadmapper
