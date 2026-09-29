#include <gtest/gtest.h>
#include "project/Project.hpp"
#include "project/ProjectSerializer.hpp"

#include <filesystem>
#include <string>

namespace quadmapper {
namespace test {

TEST(ProjectTest, NewProjectHasDefaultName) {
    Project project;

    EXPECT_EQ(project.getName(), "Untitled Project");
}

TEST(ProjectTest, SetNameUpdatesName) {
    Project project;
    project.setName("My Project");

    EXPECT_EQ(project.getName(), "My Project");
}

// Unsaved-changes tracking (ProjectSession::isProjectDirty) compares serialized content
// rather than a flag on Project (see editor/UnsavedIndicatorTest.cpp); an edit like
// addQuad must actually change what gets serialized for that comparison to work.
TEST(ProjectTest, AddQuadChangesSerializedContent) {
    Project project;
    const std::string before = ProjectSerializer().serialize(project);
    Quad quad;
    project.addQuad(quad);

    EXPECT_NE(ProjectSerializer().serialize(project), before);
}

TEST(ProjectTest, HasEmptyFilePathByDefault) {
    Project project;

    EXPECT_TRUE(project.getFilePath().empty());

    project.setFilePath("C:/test/project.qmap");

    EXPECT_FALSE(project.getFilePath().empty());
    EXPECT_EQ(project.getFilePath(), "C:/test/project.qmap");
}

TEST(ProjectTest, NewProjectHasDefaultSettings) {
    Project project;

    const ProjectSettings& settings = project.getSettings();
    EXPECT_EQ(settings.defaultResolution, IVec2(1920, 1080));
    EXPECT_EQ(settings.gridSize, 32);
    EXPECT_FALSE(settings.snapToGrid);
}

TEST(ProjectTest, QuadManagement) {
    Project project;

    EXPECT_TRUE(project.getQuads().empty());

    Quad quad;
    quad.setName("Test Quad");
    project.addQuad(quad);

    EXPECT_EQ(project.getQuads().size(), 1);
    EXPECT_EQ(project.getQuads()[0].getName(), "Test Quad");

    project.clearQuads();
    EXPECT_TRUE(project.getQuads().empty());
}

TEST(ProjectTest, TexturePathsParallelToQuads) {
    Project project;

    project.getQuads().push_back(Quad());
    project.getTexturePaths().push_back("texture1.png");

    project.getQuads().push_back(Quad());
    project.getTexturePaths().push_back("texture2.png");

    EXPECT_EQ(project.getQuads().size(), project.getTexturePaths().size());
    EXPECT_EQ(project.getTexturePaths()[0], "texture1.png");
    EXPECT_EQ(project.getTexturePaths()[1], "texture2.png");
}

// Serialization tests
TEST(ProjectSerializerTest, SerializeEmptyProject) {
    Project project;
    project.setName("Test Project");

    std::string json = ProjectSerializer().serialize(project);

    EXPECT_FALSE(json.empty());
    EXPECT_NE(json.find("Test Project"), std::string::npos);
    // Writer version 1.2.0: additive colorCorrection (ADR-036) + control grid (ADR-038).
    EXPECT_NE(json.find("1.2.0"), std::string::npos);
}

TEST(ProjectSerializerTest, SerializeDeserializeRoundTrip) {
    Project original;
    original.setName("Round Trip Test");

    Quad quad;
    quad.setName("Test Quad");
    quad.setOpacity(0.75f);
    quad.getGeometry().setMeshResolution(8);

    std::array<Vec2, 4> corners = {
        Vec2(0.1f, 0.1f),
        Vec2(0.9f, 0.15f),
        Vec2(0.85f, 0.9f),
        Vec2(0.15f, 0.85f)
    };
    quad.getGeometry().setCorners(corners);

    original.getQuads().push_back(quad);
    original.getTexturePaths().push_back("test_texture.png");

    // Serialize
    std::string json = ProjectSerializer().serialize(original);

    // Deserialize
    Project loaded;
    auto result = ProjectSerializer().deserialize(json, loaded);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(loaded.getName(), "Round Trip Test");
    EXPECT_EQ(loaded.getQuads().size(), 1);
    EXPECT_EQ(loaded.getQuads()[0].getName(), "Test Quad");
    EXPECT_FLOAT_EQ(loaded.getQuads()[0].getOpacity(), 0.75f);
    EXPECT_EQ(loaded.getQuads()[0].getGeometry().getMeshResolution(), 8);
    EXPECT_EQ(loaded.getTexturePaths()[0], "test_texture.png");

    // Check corners
    const auto& loadedCorners = loaded.getQuads()[0].getGeometry().getCorners();
    EXPECT_FLOAT_EQ(loadedCorners[0].x, 0.1f);
    EXPECT_FLOAT_EQ(loadedCorners[0].y, 0.1f);
    EXPECT_FLOAT_EQ(loadedCorners[1].x, 0.9f);
    EXPECT_FLOAT_EQ(loadedCorners[1].y, 0.15f);
}

TEST(ProjectSerializerTest, DeserializeEmptyStringFails) {
    Project project;
    auto result = ProjectSerializer().deserialize("", project);

    EXPECT_TRUE(result.isError());
}

TEST(ProjectSerializerTest, DeserializeInvalidJsonFails) {
    Project project;
    auto result = ProjectSerializer().deserialize("{not valid json", project);

    EXPECT_TRUE(result.isError());
}

TEST(ProjectSerializerTest, DeserializeMissingVersionFails) {
    Project project;
    auto result = ProjectSerializer().deserialize("{\"name\": \"Test\"}", project);

    EXPECT_TRUE(result.isError());
}

} // namespace test
} // namespace quadmapper
