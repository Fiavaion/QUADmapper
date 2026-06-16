#include <gtest/gtest.h>
#include "project/ProjectSerializer.hpp"
#include "project/Project.hpp"
#include "scene/Quad.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneManager.hpp"

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

// ============ ADR-032: version policy & migration ============

// A file from a newer major than this build supports must be rejected loudly (on every read
// path), not silently half-parsed. Previously the scene paths ignored version entirely.
TEST(ProjectSerializerVersionTest, FutureMajorVersionRejectedFlat) {
    ProjectSerializer serializer;
    Project project;

    std::string json = R"({"version": "3.0.0", "name": "FromTheFuture", "quads": []})";
    Result<void, std::string> result = serializer.deserialize(json, project);

    EXPECT_TRUE(result.isError());
}

TEST(ProjectSerializerVersionTest, FutureMajorVersionRejectedScenes) {
    ProjectSerializer serializer;
    std::string projectName;
    SceneManager sceneManager;

    std::string json = R"({"version": "3.0.0", "name": "FromTheFuture", "scenes": []})";
    Result<void, std::string> result = serializer.deserializeScenes(json, projectName, sceneManager);

    EXPECT_TRUE(result.isError());
}

TEST(ProjectSerializerVersionTest, MalformedVersionRejected) {
    ProjectSerializer serializer;
    Project project;

    std::string json = R"({"version": "not-a-version", "name": "Test"})";
    Result<void, std::string> result = serializer.deserialize(json, project);

    EXPECT_TRUE(result.isError());
}

TEST(ProjectSerializerVersionTest, ScenePathRejectsMissingVersion) {
    ProjectSerializer serializer;
    std::string projectName;
    SceneManager sceneManager;

    // Previously loadToSceneManager/deserializeScenes accepted a missing version silently.
    std::string json = R"({"name": "NoVersion", "scenes": []})";
    Result<void, std::string> result = serializer.deserializeScenes(json, projectName, sceneManager);

    EXPECT_TRUE(result.isError());
}

// A v1.x flat "quads" document loaded through the scene path migrates into a single scene.
TEST(ProjectSerializerVersionTest, V1FlatMigratesToSingleScene) {
    ProjectSerializer serializer;
    std::string projectName;
    SceneManager sceneManager;

    std::string json = R"({
        "version": "1.0.0",
        "name": "Legacy",
        "quads": [
            {"name": "A", "geometry": {"corners": [[0,0],[1,0],[1,1],[0,1]], "meshResolution": 8, "meshMode": 0}},
            {"name": "B", "geometry": {"corners": [[0,0],[1,0],[1,1],[0,1]], "meshResolution": 8, "meshMode": 0}}
        ]
    })";

    Result<void, std::string> result = serializer.deserializeScenes(json, projectName, sceneManager);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(projectName, "Legacy");
    EXPECT_EQ(sceneManager.getSceneCount(), 1);
    Scene* scene = sceneManager.getSceneByIndex(0);
    ASSERT_NE(scene, nullptr);
    EXPECT_EQ(scene->getQuads().size(), 2u);
}

// A v2.x scene document loaded into the flat Project model flattens scene quads, instead of
// loading silently empty (which is what the old one-way version reject produced).
TEST(ProjectSerializerVersionTest, V2ScenesFlattenIntoFlatProject) {
    ProjectSerializer serializer;

    // Build a 2-scene project and serialize it in scene format.
    SceneManager sceneManager;
    sceneManager.getSceneByIndex(0)->createQuad()->setName("S1Q1");
    Scene* scene2 = sceneManager.createScene("Scene 2");
    scene2->createQuad()->setName("S2Q1");
    scene2->createQuad()->setName("S2Q2");

    std::string json = serializer.serializeScenes("MultiScene", sceneManager);

    Project flat;
    Result<void, std::string> result = serializer.deserialize(json, flat);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(flat.getName(), "MultiScene");
    EXPECT_EQ(flat.getQuads().size(), 3u);  // 1 + 2 flattened
}

// Mesh-edit warps (vertex offsets) must survive a scene round-trip. The pre-refactor scene
// loader copied only corners/resolution/mode and silently dropped mesh offsets.
TEST(ProjectSerializerVersionTest, MeshOffsetsSurviveSceneRoundTrip) {
    ProjectSerializer serializer;

    SceneManager sceneManager;
    Quad* quad = sceneManager.getSceneByIndex(0)->createQuad();
    quad->setName("Warped");
    QuadGeometry& geom = quad->getGeometry();
    geom.setMeshMode(MeshMode::MESH_EDIT);
    ASSERT_GT(geom.getVertexCount(), 0);
    geom.setMeshVertexByIndex(0, Vec2(0.07f, -0.04f));
    ASSERT_TRUE(geom.hasMeshModifications());

    std::string json = serializer.serializeScenes("MeshProj", sceneManager);

    SceneManager loaded;
    std::string loadedName;
    Result<void, std::string> result = serializer.deserializeScenes(json, loadedName, loaded);

    ASSERT_TRUE(result.isSuccess());
    Scene* scene = loaded.getSceneByIndex(0);
    ASSERT_NE(scene, nullptr);
    ASSERT_EQ(scene->getQuads().size(), 1u);
    const QuadGeometry& loadedGeom = scene->getQuads()[0]->getGeometry();
    EXPECT_TRUE(loadedGeom.hasMeshModifications());
    Vec2 offset = loadedGeom.getMeshVertexByIndex(0);
    EXPECT_NEAR(offset.x, 0.07f, 0.0001f);
    EXPECT_NEAR(offset.y, -0.04f, 0.0001f);
}

} // namespace test
} // namespace quadmapper
