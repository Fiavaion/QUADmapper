#include <gtest/gtest.h>
#include "scene/Quad.hpp"

namespace quadmapper {
namespace test {

TEST(QuadTest, DefaultConstructorCreatesValidQuad) {
    Quad quad;

    EXPECT_FALSE(quad.getId().isNull());
    EXPECT_EQ(quad.getName(), "New Quad");
    EXPECT_TRUE(quad.isVisible());
    EXPECT_FALSE(quad.isLocked());
}

TEST(QuadTest, SetNameUpdatesName) {
    Quad quad;
    quad.setName("TestQuad");

    EXPECT_EQ(quad.getName(), "TestQuad");
}

TEST(QuadTest, SetVisibilityUpdatesState) {
    Quad quad;

    quad.setVisible(false);
    EXPECT_FALSE(quad.isVisible());

    quad.setVisible(true);
    EXPECT_TRUE(quad.isVisible());
}

TEST(QuadTest, SetLockedUpdatesState) {
    Quad quad;

    quad.setLocked(true);
    EXPECT_TRUE(quad.isLocked());

    quad.setLocked(false);
    EXPECT_FALSE(quad.isLocked());
}

TEST(QuadTest, SetOpacityClamps) {
    Quad quad;

    quad.setOpacity(0.5f);
    EXPECT_FLOAT_EQ(quad.getOpacity(), 0.5f);

    quad.setOpacity(-0.5f);
    EXPECT_FLOAT_EQ(quad.getOpacity(), 0.0f);

    quad.setOpacity(1.5f);
    EXPECT_FLOAT_EQ(quad.getOpacity(), 1.0f);
}

TEST(QuadTest, SetZOrderUpdatesValue) {
    Quad quad;
    quad.setZOrder(5);

    EXPECT_EQ(quad.getZOrder(), 5);
}

TEST(QuadTest, DefaultOpacityIsOne) {
    Quad quad;

    EXPECT_FLOAT_EQ(quad.getOpacity(), 1.0f);
}

TEST(QuadTest, DefaultZOrderIsZero) {
    Quad quad;

    EXPECT_EQ(quad.getZOrder(), 0);
}

TEST(QuadTest, GeometryAccessible) {
    Quad quad;

    // Should be able to get geometry
    QuadGeometry& geometry = quad.getGeometry();

    // Should have valid mesh resolution
    EXPECT_GE(geometry.getMeshResolution(), MIN_MESH_RESOLUTION);
    EXPECT_LE(geometry.getMeshResolution(), MAX_MESH_RESOLUTION);
}

TEST(QuadTest, OutputAssignments) {
    Quad quad;
    UUID outputId = UUID::generate();

    EXPECT_FALSE(quad.isAssignedToOutput(outputId));

    quad.assignToOutput(outputId);
    EXPECT_TRUE(quad.isAssignedToOutput(outputId));

    quad.removeFromOutput(outputId);
    EXPECT_FALSE(quad.isAssignedToOutput(outputId));
}

// TextureBinding UV Transform Tests

TEST(TextureBindingTest, DefaultValues) {
    TextureBinding binding;

    EXPECT_FLOAT_EQ(binding.uvOffset.x, 0.0f);
    EXPECT_FLOAT_EQ(binding.uvOffset.y, 0.0f);
    EXPECT_FLOAT_EQ(binding.uvScale.x, 1.0f);
    EXPECT_FLOAT_EQ(binding.uvScale.y, 1.0f);
    EXPECT_FLOAT_EQ(binding.uvRotation, 0.0f);
    EXPECT_FALSE(binding.flipH);
    EXPECT_FALSE(binding.flipV);
}

TEST(TextureBindingTest, ResetUV) {
    TextureBinding binding;

    // Modify values
    binding.uvOffset = Vec2(0.5f, -0.3f);
    binding.uvScale = Vec2(2.0f, 0.5f);
    binding.uvRotation = 1.57f; // ~90 degrees

    // Reset
    binding.resetUV();

    // Verify defaults restored
    EXPECT_FLOAT_EQ(binding.uvOffset.x, 0.0f);
    EXPECT_FLOAT_EQ(binding.uvOffset.y, 0.0f);
    EXPECT_FLOAT_EQ(binding.uvScale.x, 1.0f);
    EXPECT_FLOAT_EQ(binding.uvScale.y, 1.0f);
    EXPECT_FLOAT_EQ(binding.uvRotation, 0.0f);
}

TEST(TextureBindingTest, UVOffsetModification) {
    TextureBinding binding;

    binding.uvOffset = Vec2(0.25f, -0.5f);

    EXPECT_FLOAT_EQ(binding.uvOffset.x, 0.25f);
    EXPECT_FLOAT_EQ(binding.uvOffset.y, -0.5f);
}

TEST(TextureBindingTest, UVScaleModification) {
    TextureBinding binding;

    binding.uvScale = Vec2(2.0f, 0.5f);

    EXPECT_FLOAT_EQ(binding.uvScale.x, 2.0f);
    EXPECT_FLOAT_EQ(binding.uvScale.y, 0.5f);
}

TEST(TextureBindingTest, UVRotationModification) {
    TextureBinding binding;

    binding.uvRotation = 3.14159f;

    EXPECT_NEAR(binding.uvRotation, 3.14159f, 0.0001f);
}

TEST(QuadTest, TextureBindingAccessible) {
    Quad quad;

    // Should be able to get texture binding
    TextureBinding& texBinding = quad.getTexture();

    // Should have default values
    EXPECT_FLOAT_EQ(texBinding.uvOffset.x, 0.0f);
    EXPECT_FLOAT_EQ(texBinding.uvScale.x, 1.0f);
    EXPECT_FLOAT_EQ(texBinding.uvRotation, 0.0f);
}

TEST(QuadTest, ModifyTextureBindingUV) {
    Quad quad;

    TextureBinding& texBinding = quad.getTexture();
    texBinding.uvOffset = Vec2(0.1f, 0.2f);
    texBinding.uvScale = Vec2(1.5f, 2.0f);
    texBinding.uvRotation = 0.5f;

    // Verify changes persisted
    const TextureBinding& constTexBinding = quad.getTexture();
    EXPECT_FLOAT_EQ(constTexBinding.uvOffset.x, 0.1f);
    EXPECT_FLOAT_EQ(constTexBinding.uvOffset.y, 0.2f);
    EXPECT_FLOAT_EQ(constTexBinding.uvScale.x, 1.5f);
    EXPECT_FLOAT_EQ(constTexBinding.uvScale.y, 2.0f);
    EXPECT_FLOAT_EQ(constTexBinding.uvRotation, 0.5f);
}

} // namespace test
} // namespace quadmapper
