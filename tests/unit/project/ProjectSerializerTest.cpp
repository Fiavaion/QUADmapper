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

// ADR-036: per-quad color correction survives a save/load round-trip.
TEST(ProjectSerializerExtraTest, ColorCorrectionRoundTrip) {
    ProjectSerializer serializer;

    Project originalProject;
    originalProject.setName("Color Test Project");

    Quad quad;
    quad.setName("Color Quad");
    ColorCorrection cc;
    cc.brightness = 0.3f;
    cc.contrast = 1.4f;
    cc.gamma = 2.2f;
    cc.rgbGain = Vec3(1.1f, 0.9f, 0.75f);
    quad.setColorCorrection(cc);

    originalProject.getQuads().push_back(std::move(quad));
    originalProject.getTexturePaths().push_back("");

    std::string json = serializer.serialize(originalProject);
    Project loadedProject;
    Result<void, std::string> result = serializer.deserialize(json, loadedProject);

    EXPECT_TRUE(result.isSuccess());
    ASSERT_EQ(loadedProject.getQuads().size(), 1u);
    const ColorCorrection& loaded = loadedProject.getQuads()[0].getColorCorrection();
    EXPECT_NEAR(loaded.brightness, 0.3f, 0.0001f);
    EXPECT_NEAR(loaded.contrast, 1.4f, 0.0001f);
    EXPECT_NEAR(loaded.gamma, 2.2f, 0.0001f);
    EXPECT_NEAR(loaded.rgbGain.x, 1.1f, 0.0001f);
    EXPECT_NEAR(loaded.rgbGain.y, 0.9f, 0.0001f);
    EXPECT_NEAR(loaded.rgbGain.z, 0.75f, 0.0001f);
}

// ADR-036: an identity correction is omitted from disk and an older file
// without the field loads as identity (backward compatible).
TEST(ProjectSerializerExtraTest, ColorCorrectionIdentityOmittedAndBackwardCompatible) {
    ProjectSerializer serializer;

    Project originalProject;
    originalProject.setName("Identity Color Project");
    Quad quad;
    quad.setName("Plain Quad");  // leave color correction at identity defaults
    originalProject.getQuads().push_back(std::move(quad));
    originalProject.getTexturePaths().push_back("");

    std::string json = serializer.serialize(originalProject);
    // Identity must not be written (keeps untouched scenes byte-clean).
    EXPECT_EQ(json.find("colorCorrection"), std::string::npos);

    Project loadedProject;
    EXPECT_TRUE(serializer.deserialize(json, loadedProject).isSuccess());
    EXPECT_TRUE(loadedProject.getQuads()[0].getColorCorrection().isIdentity());
}

// ADR-037: per-quad soft-edge blending survives a save/load round-trip.
TEST(ProjectSerializerExtraTest, SoftEdgeRoundTrip) {
    ProjectSerializer serializer;

    Project originalProject;
    originalProject.setName("Soft Edge Project");

    Quad quad;
    quad.setName("Feathered Quad");
    SoftEdge se;
    se.left = 0.2f;
    se.right = 0.15f;
    se.top = 0.05f;
    se.bottom = 0.3f;
    se.gamma = 1.8f;
    quad.setSoftEdge(se);

    originalProject.getQuads().push_back(std::move(quad));
    originalProject.getTexturePaths().push_back("");

    std::string json = serializer.serialize(originalProject);
    Project loadedProject;
    Result<void, std::string> result = serializer.deserialize(json, loadedProject);

    EXPECT_TRUE(result.isSuccess());
    ASSERT_EQ(loadedProject.getQuads().size(), 1u);
    const SoftEdge& loaded = loadedProject.getQuads()[0].getSoftEdge();
    EXPECT_NEAR(loaded.left, 0.2f, 0.0001f);
    EXPECT_NEAR(loaded.right, 0.15f, 0.0001f);
    EXPECT_NEAR(loaded.top, 0.05f, 0.0001f);
    EXPECT_NEAR(loaded.bottom, 0.3f, 0.0001f);
    EXPECT_NEAR(loaded.gamma, 1.8f, 0.0001f);
}

// ADR-037: an identity (no-feather) soft edge is omitted from disk and an older
// file without the field loads as identity.
TEST(ProjectSerializerExtraTest, SoftEdgeIdentityOmittedAndBackwardCompatible) {
    ProjectSerializer serializer;

    Project originalProject;
    originalProject.setName("No Feather Project");
    Quad quad;
    quad.setName("Plain Quad");  // leave soft edge at identity defaults
    originalProject.getQuads().push_back(std::move(quad));
    originalProject.getTexturePaths().push_back("");

    std::string json = serializer.serialize(originalProject);
    EXPECT_EQ(json.find("softEdge"), std::string::npos);

    Project loadedProject;
    EXPECT_TRUE(serializer.deserialize(json, loadedProject).isSuccess());
    EXPECT_TRUE(loadedProject.getQuads()[0].getSoftEdge().isIdentity());
}

// ADR-038: a non-uniform control grid survives a save/load round-trip; a plain
// uniform grid stays byte-clean (no uCoords written) and still loads.
TEST(ProjectSerializerExtraTest, NonUniformGridRoundTrip) {
    ProjectSerializer serializer;

    Project originalProject;
    originalProject.setName("Grid Project");
    Quad quad;
    quad.setName("Grid Quad");
    quad.getGeometry().setUniformGrid(2);          // 0, 0.5, 1
    quad.getGeometry().insertUDivision(0.25f);     // -> 0, 0.25, 0.5, 1
    quad.getGeometry().insertVDivision(0.8f);
    ASSERT_FALSE(quad.getGeometry().isUniform());
    originalProject.getQuads().push_back(std::move(quad));
    originalProject.getTexturePaths().push_back("");

    std::string json = serializer.serialize(originalProject);
    EXPECT_NE(json.find("uCoords"), std::string::npos);

    Project loaded;
    ASSERT_TRUE(serializer.deserialize(json, loaded).isSuccess());
    const auto& g = loaded.getQuads()[0].getGeometry();
    EXPECT_FALSE(g.isUniform());
    ASSERT_EQ(g.getUCoords().size(), 4u);
    EXPECT_NEAR(g.getUCoord(1), 0.25f, 1e-4f);
    EXPECT_NEAR(g.getVCoords().back(), 1.0f, 1e-4f);
}

TEST(ProjectSerializerExtraTest, UniformGridOmitsCoordsButLoads) {
    ProjectSerializer serializer;
    Project p;
    p.setName("Uniform");
    Quad q;
    q.getGeometry().setUniformGrid(8);
    p.getQuads().push_back(std::move(q));
    p.getTexturePaths().push_back("");

    std::string json = serializer.serialize(p);
    EXPECT_EQ(json.find("uCoords"), std::string::npos);  // byte-clean uniform

    Project loaded;
    ASSERT_TRUE(serializer.deserialize(json, loaded).isSuccess());
    EXPECT_TRUE(loaded.getQuads()[0].getGeometry().isUniform());
    EXPECT_EQ(loaded.getQuads()[0].getGeometry().getColumns(), 8);
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

TEST(ProjectSerializerVersionTest, MalformedVersionRejected) {
    ProjectSerializer serializer;
    Project project;

    std::string json = R"({"version": "not-a-version", "name": "Test"})";
    Result<void, std::string> result = serializer.deserialize(json, project);

    EXPECT_TRUE(result.isError());
}

} // namespace test
} // namespace quadmapper
