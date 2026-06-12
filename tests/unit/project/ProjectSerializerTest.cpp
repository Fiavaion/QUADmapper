#include <gtest/gtest.h>
#include "project/ProjectSerializer.hpp"
#include "project/Project.hpp"
#include "scene/Quad.hpp"

namespace quadmapper {
namespace test {

// Note: Main serialization tests are in ProjectTest.cpp
// These are additional edge case tests

TEST(ProjectSerializerExtraTest, InvalidJsonReturnsError) {
    ProjectSerializer serializer;
    Project project;

    Result<void, std::string> result = serializer.deserialize("not valid json", project);

    EXPECT_TRUE(result.isError());
}

TEST(ProjectSerializerExtraTest, MissingVersionReturnsError) {
    ProjectSerializer serializer;
    Project project;

    Result<void, std::string> result = serializer.deserialize("{}", project);

    EXPECT_TRUE(result.isError());
}

TEST(ProjectSerializerExtraTest, IncompatibleVersionReturnsError) {
    ProjectSerializer serializer;
    Project project;

    std::string json = R"({"version": "999.0.0", "name": "Test"})";
    Result<void, std::string> result = serializer.deserialize(json, project);

    EXPECT_TRUE(result.isError());
}

TEST(ProjectSerializerExtraTest, CompatibleVersion1xWorks) {
    ProjectSerializer serializer;
    Project project;

    std::string json = R"({"version": "1.5.0", "name": "Test"})";
    Result<void, std::string> result = serializer.deserialize(json, project);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(project.getName(), "Test");
}

TEST(ProjectSerializerExtraTest, QuadsArrayCanBeEmpty) {
    ProjectSerializer serializer;
    Project project;

    std::string json = R"({"version": "1.0.0", "name": "Test", "quads": []})";
    Result<void, std::string> result = serializer.deserialize(json, project);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_TRUE(project.getQuads().empty());
}

// UV Transform Serialization Tests

TEST(ProjectSerializerExtraTest, UVTransformSerializationRoundTrip) {
    ProjectSerializer serializer;

    // Create project with quad having UV transforms
    Project originalProject;
    originalProject.setName("UV Test Project");

    Quad quad;
    quad.setName("UV Quad");
    TextureBinding& texBinding = quad.getTexture();
    texBinding.uvOffset = Vec2(0.25f, -0.1f);
    texBinding.uvScale = Vec2(1.5f, 2.0f);
    texBinding.uvRotation = 0.785398f; // 45 degrees

    originalProject.getQuads().push_back(std::move(quad));
    originalProject.getTexturePaths().push_back("");

    // Serialize
    std::string json = serializer.serialize(originalProject);

    // Deserialize
    Project loadedProject;
    Result<void, std::string> result = serializer.deserialize(json, loadedProject);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(loadedProject.getQuads().size(), 1);

    const TextureBinding& loadedBinding = loadedProject.getQuads()[0].getTexture();
    EXPECT_NEAR(loadedBinding.uvOffset.x, 0.25f, 0.0001f);
    EXPECT_NEAR(loadedBinding.uvOffset.y, -0.1f, 0.0001f);
    EXPECT_NEAR(loadedBinding.uvScale.x, 1.5f, 0.0001f);
    EXPECT_NEAR(loadedBinding.uvScale.y, 2.0f, 0.0001f);
    EXPECT_NEAR(loadedBinding.uvRotation, 0.785398f, 0.0001f);
}

TEST(ProjectSerializerExtraTest, UVDefaultValuesPreserved) {
    ProjectSerializer serializer;

    // Create project with default UV values
    Project originalProject;
    originalProject.setName("Default UV Project");

    Quad quad;
    quad.setName("Default UV Quad");
    // Leave texture binding at defaults

    originalProject.getQuads().push_back(std::move(quad));
    originalProject.getTexturePaths().push_back("");

    // Serialize and deserialize
    std::string json = serializer.serialize(originalProject);
    Project loadedProject;
    Result<void, std::string> result = serializer.deserialize(json, loadedProject);

    EXPECT_TRUE(result.isSuccess());

    const TextureBinding& loadedBinding = loadedProject.getQuads()[0].getTexture();
    EXPECT_FLOAT_EQ(loadedBinding.uvOffset.x, 0.0f);
    EXPECT_FLOAT_EQ(loadedBinding.uvOffset.y, 0.0f);
    EXPECT_FLOAT_EQ(loadedBinding.uvScale.x, 1.0f);
    EXPECT_FLOAT_EQ(loadedBinding.uvScale.y, 1.0f);
    EXPECT_FLOAT_EQ(loadedBinding.uvRotation, 0.0f);
}

TEST(ProjectSerializerExtraTest, UVFlipFlagsSerialize) {
    ProjectSerializer serializer;

    // Create project with flip flags set
    Project originalProject;
    originalProject.setName("Flip Test Project");

    Quad quad;
    quad.setName("Flip Quad");
    TextureBinding& texBinding = quad.getTexture();
    texBinding.flipH = true;
    texBinding.flipV = true;

    originalProject.getQuads().push_back(std::move(quad));
    originalProject.getTexturePaths().push_back("");

    // Serialize and deserialize
    std::string json = serializer.serialize(originalProject);
    Project loadedProject;
    Result<void, std::string> result = serializer.deserialize(json, loadedProject);

    EXPECT_TRUE(result.isSuccess());

    const TextureBinding& loadedBinding = loadedProject.getQuads()[0].getTexture();
    EXPECT_TRUE(loadedBinding.flipH);
    EXPECT_TRUE(loadedBinding.flipV);
}

TEST(ProjectSerializerExtraTest, BackwardCompatibilityNoTextureBinding) {
    ProjectSerializer serializer;
    Project project;

    // JSON from older version without textureBinding field
    std::string json = R"({
        "version": "1.0.0",
        "name": "Old Project",
        "quads": [{
            "id": "test-uuid",
            "name": "Old Quad",
            "zOrder": 0,
            "visible": true,
            "locked": false,
            "opacity": 1.0,
            "geometry": {
                "corners": [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]],
                "meshResolution": 16,
                "meshMode": 0
            }
        }]
    })";

    Result<void, std::string> result = serializer.deserialize(json, project);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(project.getQuads().size(), 1);

    // Should have default UV values
    const TextureBinding& loadedBinding = project.getQuads()[0].getTexture();
    EXPECT_FLOAT_EQ(loadedBinding.uvOffset.x, 0.0f);
    EXPECT_FLOAT_EQ(loadedBinding.uvOffset.y, 0.0f);
    EXPECT_FLOAT_EQ(loadedBinding.uvScale.x, 1.0f);
    EXPECT_FLOAT_EQ(loadedBinding.uvScale.y, 1.0f);
    EXPECT_FLOAT_EQ(loadedBinding.uvRotation, 0.0f);
}

} // namespace test
} // namespace quadmapper
