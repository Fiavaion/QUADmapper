#include <gtest/gtest.h>
#include "scene/MeshSubdivision.hpp"
#include "scene/QuadGeometry.hpp"

namespace quadmapper {
namespace test {

// =============================================================================
// Basic Mesh Generation Tests
// =============================================================================

TEST(MeshSubdivisionTest, ResolutionTwoReturnsNineVertices) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    // 2 resolution = 3x3 grid = 9 vertices
    EXPECT_EQ(vertices.size(), 9);
}

TEST(MeshSubdivisionTest, ResolutionFourReturnsTwentyFiveVertices) {
    QuadGeometry geometry;
    geometry.setMeshResolution(4);

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    // 4 resolution = 5x5 grid = 25 vertices
    EXPECT_EQ(vertices.size(), 25);
}

TEST(MeshSubdivisionTest, SubdivisionCreatesCorrectIndices) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    // 2 resolution = 2x2 quads = 4 quads = 8 triangles = 24 indices
    EXPECT_EQ(indices.size(), 24);
}

TEST(MeshSubdivisionTest, VertexPositionsAreInterpolated) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    // Set up unit square
    geometry.setCorners({
        Vec2(0.0f, 0.0f),
        Vec2(1.0f, 0.0f),
        Vec2(1.0f, 1.0f),
        Vec2(0.0f, 1.0f)
    });

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    // Center vertex should be at (0.5, 0.5)
    // For 3x3 grid, center is index 4
    EXPECT_NEAR(vertices[4].position.x, 0.5f, 0.001f);
    EXPECT_NEAR(vertices[4].position.y, 0.5f, 0.001f);
}

TEST(MeshSubdivisionTest, UVCoordinatesAreCorrect) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    // Top-left corner UV should be (0, 0)
    EXPECT_NEAR(vertices[0].texCoord.x, 0.0f, 0.001f);
    EXPECT_NEAR(vertices[0].texCoord.y, 0.0f, 0.001f);

    // Bottom-right corner UV should be (1, 1) - index 8 for 3x3 grid
    EXPECT_NEAR(vertices[8].texCoord.x, 1.0f, 0.001f);
    EXPECT_NEAR(vertices[8].texCoord.y, 1.0f, 0.001f);

    // Center UV should be (0.5, 0.5)
    EXPECT_NEAR(vertices[4].texCoord.x, 0.5f, 0.001f);
    EXPECT_NEAR(vertices[4].texCoord.y, 0.5f, 0.001f);
}

TEST(MeshSubdivisionTest, GetVertexIndexWorks) {
    // For resolution 2 (3x3 grid):
    // 0 1 2
    // 3 4 5
    // 6 7 8
    EXPECT_EQ(MeshSubdivision::getVertexIndex(0, 0, 2), 0);
    EXPECT_EQ(MeshSubdivision::getVertexIndex(0, 2, 2), 2);
    EXPECT_EQ(MeshSubdivision::getVertexIndex(1, 1, 2), 4);
    EXPECT_EQ(MeshSubdivision::getVertexIndex(2, 2, 2), 8);
}

TEST(MeshSubdivisionTest, GeneratePositionsMatchesQuadGeometry) {
    QuadGeometry geometry;
    geometry.setMeshResolution(4);

    auto positions = MeshSubdivision::generatePositions(geometry);
    auto directPositions = geometry.generateMeshPositions();

    EXPECT_EQ(positions.size(), directPositions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        EXPECT_FLOAT_EQ(positions[i].x, directPositions[i].x);
        EXPECT_FLOAT_EQ(positions[i].y, directPositions[i].y);
    }
}

TEST(MeshSubdivisionTest, IndicesFormValidTriangles) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    // All indices should be valid vertex indices
    for (uint32_t idx : indices) {
        EXPECT_LT(idx, vertices.size());
    }

    // Indices should be in groups of 3 (triangles)
    EXPECT_EQ(indices.size() % 3, 0);
}

// =============================================================================
// Edge Case Tests (Min/Max Resolution)
// =============================================================================

TEST(MeshSubdivisionTest, MinResolutionGeneratesCorrectMesh) {
    QuadGeometry geometry;
    geometry.setMeshResolution(MIN_MESH_RESOLUTION);

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    // Min resolution (2) = 3x3 grid = 9 vertices
    int expectedVertices = MeshSubdivision::getVertexCount(MIN_MESH_RESOLUTION);
    EXPECT_EQ(vertices.size(), static_cast<size_t>(expectedVertices));

    // 2x2 quads = 8 triangles = 24 indices
    int expectedIndices = MeshSubdivision::getIndexCount(MIN_MESH_RESOLUTION);
    EXPECT_EQ(indices.size(), static_cast<size_t>(expectedIndices));
}

TEST(MeshSubdivisionTest, MaxResolutionGeneratesCorrectMesh) {
    QuadGeometry geometry;
    geometry.setMeshResolution(MAX_MESH_RESOLUTION);

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    // Max resolution (64) = 65x65 grid = 4225 vertices
    int expectedVertices = MeshSubdivision::getVertexCount(MAX_MESH_RESOLUTION);
    EXPECT_EQ(vertices.size(), static_cast<size_t>(expectedVertices));

    // 64x64 quads = 8192 triangles = 24576 indices
    int expectedIndices = MeshSubdivision::getIndexCount(MAX_MESH_RESOLUTION);
    EXPECT_EQ(indices.size(), static_cast<size_t>(expectedIndices));
}

// =============================================================================
// Utility Function Tests
// =============================================================================

TEST(MeshSubdivisionTest, GetVertexCountReturnsCorrectValue) {
    EXPECT_EQ(MeshSubdivision::getVertexCount(2), 9);      // 3x3
    EXPECT_EQ(MeshSubdivision::getVertexCount(4), 25);     // 5x5
    EXPECT_EQ(MeshSubdivision::getVertexCount(8), 81);     // 9x9
    EXPECT_EQ(MeshSubdivision::getVertexCount(16), 289);   // 17x17
    EXPECT_EQ(MeshSubdivision::getVertexCount(64), 4225);  // 65x65
}

TEST(MeshSubdivisionTest, GetIndexCountReturnsCorrectValue) {
    EXPECT_EQ(MeshSubdivision::getIndexCount(2), 24);       // 2x2 quads x 6
    EXPECT_EQ(MeshSubdivision::getIndexCount(4), 96);       // 4x4 quads x 6
    EXPECT_EQ(MeshSubdivision::getIndexCount(8), 384);      // 8x8 quads x 6
    EXPECT_EQ(MeshSubdivision::getIndexCount(16), 1536);    // 16x16 quads x 6
    EXPECT_EQ(MeshSubdivision::getIndexCount(64), 24576);   // 64x64 quads x 6
}

TEST(MeshSubdivisionTest, GetTriangleCountReturnsCorrectValue) {
    EXPECT_EQ(MeshSubdivision::getTriangleCount(2), 8);     // 2x2 quads x 2
    EXPECT_EQ(MeshSubdivision::getTriangleCount(4), 32);    // 4x4 quads x 2
    EXPECT_EQ(MeshSubdivision::getTriangleCount(64), 8192); // 64x64 quads x 2
}

TEST(MeshSubdivisionTest, ClampResolutionClampsToValidRange) {
    EXPECT_EQ(MeshSubdivision::clampResolution(0), MIN_MESH_RESOLUTION);
    EXPECT_EQ(MeshSubdivision::clampResolution(1), MIN_MESH_RESOLUTION);
    EXPECT_EQ(MeshSubdivision::clampResolution(MIN_MESH_RESOLUTION), MIN_MESH_RESOLUTION);
    EXPECT_EQ(MeshSubdivision::clampResolution(32), 32);
    EXPECT_EQ(MeshSubdivision::clampResolution(MAX_MESH_RESOLUTION), MAX_MESH_RESOLUTION);
    EXPECT_EQ(MeshSubdivision::clampResolution(100), MAX_MESH_RESOLUTION);
    EXPECT_EQ(MeshSubdivision::clampResolution(-5), MIN_MESH_RESOLUTION);
}

TEST(MeshSubdivisionTest, IsValidResolutionReturnsTrueForValidValues) {
    EXPECT_TRUE(MeshSubdivision::isValidResolution(MIN_MESH_RESOLUTION));
    EXPECT_TRUE(MeshSubdivision::isValidResolution(16));
    EXPECT_TRUE(MeshSubdivision::isValidResolution(32));
    EXPECT_TRUE(MeshSubdivision::isValidResolution(MAX_MESH_RESOLUTION));
}

TEST(MeshSubdivisionTest, IsValidResolutionReturnsFalseForInvalidValues) {
    EXPECT_FALSE(MeshSubdivision::isValidResolution(0));
    EXPECT_FALSE(MeshSubdivision::isValidResolution(1));
    EXPECT_FALSE(MeshSubdivision::isValidResolution(MIN_MESH_RESOLUTION - 1));
    EXPECT_FALSE(MeshSubdivision::isValidResolution(MAX_MESH_RESOLUTION + 1));
    EXPECT_FALSE(MeshSubdivision::isValidResolution(100));
    EXPECT_FALSE(MeshSubdivision::isValidResolution(-5));
}

// =============================================================================
// Bilinear Position Tests
// =============================================================================

TEST(MeshSubdivisionTest, GenerateBilinearPositionsIgnoresMeshOffsets) {
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
    geometry.setMeshVertex(1, 1, Vec2(0.1f, 0.1f));

    // Get bilinear positions (should ignore offset)
    auto bilinearPositions = MeshSubdivision::generateBilinearPositions(geometry);

    // Center should still be at (0.5, 0.5) since offsets are ignored
    int centerIndex = MeshSubdivision::getVertexIndex(1, 1, 2);
    EXPECT_NEAR(bilinearPositions[centerIndex].x, 0.5f, 0.001f);
    EXPECT_NEAR(bilinearPositions[centerIndex].y, 0.5f, 0.001f);
}

TEST(MeshSubdivisionTest, GenerateIndicesOnlyCreatesCorrectIndices) {
    std::vector<uint32_t> indices;
    MeshSubdivision::generateIndices(4, indices);

    // 4x4 quads = 16 quads = 32 triangles = 96 indices
    EXPECT_EQ(indices.size(), 96);

    // All indices should be valid for a 5x5 vertex grid
    for (uint32_t idx : indices) {
        EXPECT_LT(idx, 25u);
    }
}

// =============================================================================
// Mesh Offset Tests (for future mesh editing)
// =============================================================================

TEST(MeshSubdivisionTest, MeshOffsetsAppliedInMeshEditMode) {
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
    Vec2 offset(0.1f, 0.2f);
    geometry.setMeshVertex(1, 1, offset);

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    // Center should be at (0.5 + 0.1, 0.5 + 0.2) = (0.6, 0.7)
    int centerIndex = MeshSubdivision::getVertexIndex(1, 1, 2);
    EXPECT_NEAR(vertices[centerIndex].position.x, 0.6f, 0.001f);
    EXPECT_NEAR(vertices[centerIndex].position.y, 0.7f, 0.001f);
}

TEST(MeshSubdivisionTest, MeshOffsetsNotAppliedInCornersOnlyMode) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);
    geometry.setMeshMode(MeshMode::CORNERS_ONLY);

    // Set up unit square
    geometry.setCorners({
        Vec2(0.0f, 0.0f),
        Vec2(1.0f, 0.0f),
        Vec2(1.0f, 1.0f),
        Vec2(0.0f, 1.0f)
    });

    // Add an offset to the center vertex (should be ignored)
    geometry.setMeshVertex(1, 1, Vec2(0.1f, 0.2f));

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    // Center should still be at (0.5, 0.5) since we're in CORNERS_ONLY mode
    int centerIndex = MeshSubdivision::getVertexIndex(1, 1, 2);
    EXPECT_NEAR(vertices[centerIndex].position.x, 0.5f, 0.001f);
    EXPECT_NEAR(vertices[centerIndex].position.y, 0.5f, 0.001f);
}

// =============================================================================
// Warped Quad Tests
// =============================================================================

TEST(MeshSubdivisionTest, WarpedQuadInterpolatesCorrectly) {
    QuadGeometry geometry;
    geometry.setMeshResolution(2);

    // Set up a trapezoid (wider at bottom)
    geometry.setCorners({
        Vec2(0.25f, 0.0f),  // TL (moved right)
        Vec2(0.75f, 0.0f),  // TR (moved left)
        Vec2(1.0f, 1.0f),   // BR (at corner)
        Vec2(0.0f, 1.0f)    // BL (at corner)
    });

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    // Check that all vertex positions are within bounds
    for (const auto& vertex : vertices) {
        EXPECT_GE(vertex.position.x, 0.0f);
        EXPECT_LE(vertex.position.x, 1.0f);
        EXPECT_GE(vertex.position.y, 0.0f);
        EXPECT_LE(vertex.position.y, 1.0f);
    }

    // The top-middle vertex (row 0, col 1) should be at (0.5, 0.0)
    int topMiddleIndex = MeshSubdivision::getVertexIndex(0, 1, 2);
    EXPECT_NEAR(vertices[topMiddleIndex].position.x, 0.5f, 0.001f);
    EXPECT_NEAR(vertices[topMiddleIndex].position.y, 0.0f, 0.001f);
}

} // namespace test
} // namespace quadmapper
