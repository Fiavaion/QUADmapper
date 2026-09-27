#include <gtest/gtest.h>
#include "scene/MeshSubdivision.hpp"
#include "scene/QuadGeometry.hpp"

#include <algorithm>

namespace quadmapper {
namespace test {

namespace {

QuadGeometry unitSquare(int resolution) {
    QuadGeometry geometry;
    geometry.setMeshResolution(resolution);
    geometry.setCorners({Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)});
    return geometry;
}

// The (u, v) a render vertex carries: its projective texture coordinate divided by q.
Vec2 uvOf(const MeshVertex& v) {
    return Vec2(v.texCoord.x, v.texCoord.y) / v.texCoord.z;
}

}  // namespace

// =============================================================================
// Render tessellation (ADR-049 §4)
// =============================================================================

TEST(MeshSubdivisionTest, CornerSurfaceTessellatesAtTheRenderMinimum) {
    QuadGeometry geometry = unitSquare(2);  // the control grid does not set density

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    const IVec2 cells = MeshSubdivision::generateMesh(geometry, vertices, indices);

    EXPECT_EQ(cells, IVec2(RENDER_MIN_SEGMENTS, RENDER_MIN_SEGMENTS));
    EXPECT_EQ(vertices.size(), static_cast<size_t>(33 * 33));
    EXPECT_EQ(indices.size(), static_cast<size_t>(32 * 32 * 6));

    // Zero residuals in mesh edit are the same surface, so the same mesh.
    geometry.setMeshMode(MeshMode::MESH_EDIT);
    std::vector<MeshVertex> editVertices;
    EXPECT_EQ(MeshSubdivision::generateMesh(geometry, editVertices, indices), cells);
    ASSERT_EQ(editVertices.size(), vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        EXPECT_EQ(editVertices[i].position, vertices[i].position);
        EXPECT_EQ(editVertices[i].texCoord, vertices[i].texCoord);
    }
}

TEST(MeshSubdivisionTest, MeshWarpSplitsEveryControlCell) {
    // max(2, ceil(32 / cells)) segments per cell: 1 -> 32, 3 -> 11 each, 20 -> 2
    // each, 64 -> 2 each.
    const struct { int cells; int segments; } cases[] = {
        {1, 32}, {3, 33}, {4, 32}, {20, 40}, {MAX_MESH_RESOLUTION, 128}};
    for (const auto& c : cases) {
        QuadGeometry geometry = unitSquare(c.cells);
        geometry.setMeshMode(MeshMode::MESH_EDIT);
        geometry.setMeshVertexByIndex(0, Vec2(0.01f, 0.0f));
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
        const IVec2 cells = MeshSubdivision::generateMesh(geometry, vertices, indices);
        EXPECT_EQ(cells, IVec2(c.segments, c.segments)) << c.cells << " cells";
        EXPECT_EQ(vertices.size(), static_cast<size_t>((c.segments + 1) * (c.segments + 1)));
        EXPECT_EQ(indices.size(), static_cast<size_t>(c.segments * c.segments * 6));
    }
}

TEST(MeshSubdivisionTest, EveryControlLineIsATessellationLine) {
    QuadGeometry geometry = unitSquare(1);
    geometry.setGridCoords({0.0f, 0.3f, 1.0f}, {0.0f, 0.55f, 0.6f, 1.0f});
    geometry.setMeshMode(MeshMode::MESH_EDIT);
    geometry.setMeshVertex(1, 1, Vec2(0.05f, -0.02f));
    const std::vector<float> us = geometry.tessellationU();
    const std::vector<float> vs = geometry.tessellationV();
    for (float u : geometry.getUCoords()) {
        EXPECT_NE(std::find(us.begin(), us.end(), u), us.end()) << u;
    }
    for (float v : geometry.getVCoords()) {
        EXPECT_NE(std::find(vs.begin(), vs.end(), v), vs.end()) << v;
    }
    EXPECT_TRUE(std::is_sorted(us.begin(), us.end()));
    EXPECT_TRUE(std::is_sorted(vs.begin(), vs.end()));
}

TEST(MeshSubdivisionTest, VertexPositionsAndUVsFollowTheSurface) {
    QuadGeometry geometry = unitSquare(2);
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    const MeshVertex& centre = vertices[16 * 33 + 16];
    EXPECT_NEAR(centre.position.x, 0.5f, 1e-6f);
    EXPECT_NEAR(centre.position.y, 0.5f, 1e-6f);
    EXPECT_NEAR(uvOf(centre).x, 0.5f, 1e-6f);
    EXPECT_NEAR(uvOf(centre).y, 0.5f, 1e-6f);
    EXPECT_NEAR(uvOf(vertices.front()).x, 0.0f, 1e-6f);
    EXPECT_NEAR(uvOf(vertices.back()).y, 1.0f, 1e-6f);
}

TEST(MeshSubdivisionTest, IndicesFormValidTriangles) {
    QuadGeometry geometry = unitSquare(3);
    geometry.setMeshMode(MeshMode::MESH_EDIT);
    geometry.setMeshVertex(1, 1, Vec2(0.02f, 0.0f));

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    for (uint32_t idx : indices) {
        EXPECT_LT(idx, vertices.size());
    }
    EXPECT_EQ(indices.size() % 3, 0);
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
    // 1x1 (single cell) is a valid grid since the ADR-038 stopgap (MIN_MESH_RESOLUTION=1).
    EXPECT_FALSE(MeshSubdivision::isValidResolution(MIN_MESH_RESOLUTION - 1));
    EXPECT_FALSE(MeshSubdivision::isValidResolution(MAX_MESH_RESOLUTION + 1));
    EXPECT_FALSE(MeshSubdivision::isValidResolution(100));
    EXPECT_FALSE(MeshSubdivision::isValidResolution(-5));
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
// Mesh residuals
// =============================================================================

TEST(MeshSubdivisionTest, MeshOffsetsAppliedInMeshEditMode) {
    QuadGeometry geometry = unitSquare(2);
    geometry.setMeshMode(MeshMode::MESH_EDIT);
    geometry.setMeshVertex(1, 1, Vec2(0.1f, 0.2f));

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    // 2 cells of 16 segments: the centre control vertex is render vertex (16, 16), and
    // keeps the base's texture coordinate while the residual moves it.
    const MeshVertex& centre = vertices[16 * 33 + 16];
    EXPECT_NEAR(centre.position.x, 0.6f, 1e-6f);
    EXPECT_NEAR(centre.position.y, 0.7f, 1e-6f);
    EXPECT_NEAR(uvOf(centre).x, 0.5f, 1e-6f);
    EXPECT_NEAR(uvOf(centre).y, 0.5f, 1e-6f);
}

TEST(MeshSubdivisionTest, MeshOffsetsNotAppliedInCornersOnlyMode) {
    QuadGeometry geometry = unitSquare(2);
    geometry.setMeshMode(MeshMode::CORNERS_ONLY);
    geometry.setMeshVertex(1, 1, Vec2(0.1f, 0.2f));

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshSubdivision::generateMesh(geometry, vertices, indices);

    EXPECT_NEAR(vertices[16 * 33 + 16].position.x, 0.5f, 1e-6f);
    EXPECT_NEAR(vertices[16 * 33 + 16].position.y, 0.5f, 1e-6f);
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

    // The trapezoid is symmetric, so u = 0.5 on the top edge is its midpoint.
    EXPECT_NEAR(vertices[16].position.x, 0.5f, 1e-6f);
    EXPECT_NEAR(vertices[16].position.y, 0.0f, 1e-6f);
}

} // namespace test
} // namespace quadmapper
