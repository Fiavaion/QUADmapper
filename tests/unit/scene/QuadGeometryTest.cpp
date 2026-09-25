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
// Bilinear Position Generation Tests
// =============================================================================

TEST(QuadGeometryTest, GenerateBilinearPositionsIgnoresMeshOffsets) {
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

    // generateBilinearPositions should ignore offsets
    auto bilinear = geometry.generateBilinearPositions();
    EXPECT_NEAR(bilinear[4].x, 0.5f, 0.001f);
    EXPECT_NEAR(bilinear[4].y, 0.5f, 0.001f);

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

TEST(QuadGeometryControlGridTest, InsertDivisionInterpolatesNeighbourOffsets) {
    QuadGeometry g;
    g.setUniformGrid(2);
    g.setMeshMode(MeshMode::MESH_EDIT);
    // Row 0: col0 offset (0,0), col1 (at u=0.5) offset (0.2, 0). Insert at u=0.25
    // -> new col between them, offset interpolated at t=0.5 -> (0.1, 0).
    g.setMeshVertex(0, 0, Vec2(0.0f, 0.0f));
    g.setMeshVertex(0, 1, Vec2(0.2f, 0.0f));
    g.insertUDivision(0.25f);
    EXPECT_NEAR(g.getMeshVertex(0, 1).x, 0.1f, 1e-4f);  // new column
    EXPECT_NEAR(g.getMeshVertex(0, 2).x, 0.2f, 1e-4f);  // old col1 shifted right
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

// --- Inverse bilinear (ADR-038 batch 3b: click-on-edge insert) --------------

TEST(QuadGeometryControlGridTest, InverseBilinearRoundTripsOnWarpedQuad) {
    // A deliberately non-rectangular (keystoned) quad: TL,TR,BR,BL.
    std::array<Vec2, 4> corners = {
        Vec2(0.20f, 0.25f),  // TL
        Vec2(0.80f, 0.15f),  // TR
        Vec2(0.90f, 0.85f),  // BR
        Vec2(0.10f, 0.75f),  // BL
    };
    QuadGeometry g;
    g.setCorners(corners);

    // For several (u,v), forward-map to a point then invert; expect (u,v) back.
    const float samples[] = {0.1f, 0.25f, 0.5f, 0.7f, 0.9f};
    for (float u : samples) {
        for (float v : samples) {
            Vec2 p = g.transformUVToPosition(Vec2(u, v));
            Vec2 uv = QuadGeometry::inverseBilinear(corners, p);
            EXPECT_NEAR(uv.x, u, 1e-3f) << "u=" << u << " v=" << v;
            EXPECT_NEAR(uv.y, v, 1e-3f) << "u=" << u << " v=" << v;
        }
    }
}

TEST(QuadGeometryControlGridTest, InverseBilinearWorksForUnitSquare) {
    std::array<Vec2, 4> unit = {
        Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)};
    Vec2 uv = QuadGeometry::inverseBilinear(unit, Vec2(0.3f, 0.8f));
    EXPECT_NEAR(uv.x, 0.3f, 1e-4f);
    EXPECT_NEAR(uv.y, 0.8f, 1e-4f);
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
    const auto samples = g.sampleSurface(n);
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
    const auto samples = g.sampleSurface(n);
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

} // namespace test
} // namespace quadmapper
