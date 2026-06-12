#include <gtest/gtest.h>
#include "scene/QuadGeometry.hpp"

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

} // namespace test
} // namespace quadmapper
