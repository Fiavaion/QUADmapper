#include <gtest/gtest.h>
#include "project/Project.hpp"
#include "project/ProjectSerializer.hpp"
#include "editor/UnsavedIndicator.hpp"

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
// rather than a flag on Project (see UnsavedIndicatorTest below); an edit like
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

// The "(modified)" label follows the unsaved-changes rule (the show no longer
// serializes to what was saved or loaded): a show just opened reads unmodified, and
// so does an edit undone back to the loaded state.
TEST(UnsavedIndicatorTest, AnOpenedShowAndAnUndoneEditAreNotModified) {
    auto loaded = ProjectSerializer::load(std::filesystem::path(QM_PROJECT_FIXTURES_DIR) /
                                          "v3.4-canvas-slices.qmap");
    ASSERT_TRUE(loaded.isSuccess());
    Project project = std::move(loaded.value());
    const std::string saved = ProjectSerializer().serialize(project);
    auto dirty = [&] { return ProjectSerializer().serialize(project) != saved; };

    UnsavedIndicator label;
    EXPECT_FALSE(label.shown(10.0, dirty)) << "just opened";

    ASSERT_FALSE(project.getScenes().empty());
    ASSERT_FALSE(project.getScenes()[0].quads.empty());
    QuadGeometry& geometry = project.getScenes()[0].quads[0].getGeometry();
    const Vec2 corner = geometry.getCorners()[0];
    geometry.setCorner(0, corner + Vec2(0.05f, 0.0f));  // an edit
    label.invalidate();
    EXPECT_TRUE(label.shown(10.1, dirty)) << "edited";

    geometry.setCorner(0, corner);  // undone
    label.invalidate();
    EXPECT_FALSE(label.shown(10.2, dirty)) << "undone back to the loaded state";
}

// Serializing every frame costs too much: without invalidate() the answer is kept
// for REFRESH_SECONDS, then asked again.
TEST(UnsavedIndicatorTest, RefreshesAtMostEveryHalfSecondUnlessInvalidated) {
    UnsavedIndicator label;
    int asked = 0;
    bool dirty = false;
    auto check = [&] { ++asked; return dirty; };
    EXPECT_FALSE(label.shown(1.0, check));
    dirty = true;
    EXPECT_FALSE(label.shown(1.2, check));
    EXPECT_EQ(asked, 1);
    EXPECT_TRUE(label.shown(1.0 + UnsavedIndicator::REFRESH_SECONDS, check));
    EXPECT_EQ(asked, 2);
    dirty = false;
    label.invalidate();
    EXPECT_FALSE(label.shown(1.6, check));
    EXPECT_EQ(asked, 3);
}

} // namespace test
} // namespace quadmapper
