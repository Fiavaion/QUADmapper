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

    // v3 is the current max (ADR-039); a v4 file must still be rejected loudly.
    std::string json = R"({"version": "4.0.0", "name": "FromTheFuture", "quads": []})";
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

// ============ ADR-039: .qmap v3 multi-scene + outputs migration ============

// Every read path yields >= 1 scene. A v1.x flat file wraps its top-level quads
// into a single scene while still populating the flat quad list (back-compat).
TEST(ProjectSerializerV3Test, V1FlatLoadsAsSingleScene) {
    ProjectSerializer serializer;
    Project project;

    std::string json = R"({
        "version": "1.0.0", "name": "Flat",
        "quads": [{"name": "Q1", "geometry": {"corners": [[0,0],[1,0],[1,1],[0,1]]}}]
    })";
    ASSERT_TRUE(serializer.deserialize(json, project).isSuccess());
    ASSERT_EQ(project.getScenes().size(), 1u);
    EXPECT_EQ(project.getScenes()[0].quads.size(), 1u);
    EXPECT_EQ(project.getQuads().size(), 1u);  // flat view still populated
}

// A v2.x file's "scenes" array maps to scene DTOs (one per scene).
TEST(ProjectSerializerV3Test, V2ScenesMapToSceneList) {
    ProjectSerializer serializer;
    Project project;

    std::string json = R"({
        "version": "2.0.0", "name": "Legacy",
        "scenes": [
            {"name": "A", "quads": [{"name": "Qa"}]},
            {"name": "B", "quads": [{"name": "Qb1"}, {"name": "Qb2"}]}
        ]
    })";
    ASSERT_TRUE(serializer.deserialize(json, project).isSuccess());
    ASSERT_EQ(project.getScenes().size(), 2u);
    EXPECT_EQ(project.getScenes()[0].name, "A");
    EXPECT_EQ(project.getScenes()[1].quads.size(), 2u);
    EXPECT_EQ(project.getQuads().size(), 3u);  // flattened union
}

// v3 is accepted; a v4 (future) major is rejected (covered above with 4.0.0).
TEST(ProjectSerializerV3Test, V3VersionAccepted) {
    ProjectSerializer serializer;
    Project project;
    std::string json = R"({"version": "3.0.0", "name": "V3", "scenes": []})";
    ASSERT_TRUE(serializer.deserialize(json, project).isSuccess());
    // An empty scenes array still yields a guaranteed default scene.
    EXPECT_EQ(project.getScenes().size(), 1u);
}

// Full v3 round-trip: multiple scenes, per-scene output assignment, and the
// outputs array all survive serialize -> deserialize.
TEST(ProjectSerializerV3Test, V3RoundTripScenesAndOutputs) {
    ProjectSerializer serializer;

    Project original;
    original.setName("Show");

    UUID outputId = UUID::generate();  // a concrete output id to link a scene to

    Project::OutputData out;
    out.id = outputId;
    out.name = "Projector 1";
    out.monitor = 1;
    out.monitorName = "EPSON PJ";
    out.monitorPosition = IVec2(-1920, 0);
    out.resolution = IVec2(1280, 720);
    out.rotationDegrees = 90;
    out.enabled = true;
    original.getOutputs().push_back(out);

    Project::SceneData s1;
    s1.id = UUID::generate();
    s1.name = "Stage";
    s1.assignedOutputId = outputId;  // routed to the projector
    s1.view.zoom = 2.5f;
    {
        Quad q; q.setName("StageQuad");
        s1.quads.push_back(std::move(q));
        s1.texturePaths.push_back("media/stage.png");
    }
    original.getScenes().push_back(std::move(s1));

    Project::SceneData s2;
    s2.id = UUID::generate();
    s2.name = "Backup";  // no output assigned (edit-only)
    original.getScenes().push_back(std::move(s2));

    std::string json = serializer.serialize(original);
    EXPECT_NE(json.find("\"version\": \"3.2.0\""), std::string::npos);
    EXPECT_NE(json.find("scenes"), std::string::npos);
    EXPECT_NE(json.find("outputs"), std::string::npos);

    Project loaded;
    ASSERT_TRUE(serializer.deserialize(json, loaded).isSuccess());

    ASSERT_EQ(loaded.getScenes().size(), 2u);
    EXPECT_EQ(loaded.getScenes()[0].name, "Stage");
    EXPECT_EQ(loaded.getScenes()[0].assignedOutputId, outputId);
    EXPECT_FALSE(loaded.getScenes()[0].assignedOutputId.isNull());
    EXPECT_NEAR(loaded.getScenes()[0].view.zoom, 2.5f, 1e-4f);
    ASSERT_EQ(loaded.getScenes()[0].quads.size(), 1u);
    EXPECT_EQ(loaded.getScenes()[0].quads[0].getName(), "StageQuad");
    EXPECT_EQ(loaded.getScenes()[0].texturePaths[0], "media/stage.png");
    EXPECT_TRUE(loaded.getScenes()[1].assignedOutputId.isNull());

    ASSERT_EQ(loaded.getOutputs().size(), 1u);
    EXPECT_EQ(loaded.getOutputs()[0].id, outputId);
    EXPECT_EQ(loaded.getOutputs()[0].name, "Projector 1");
    EXPECT_EQ(loaded.getOutputs()[0].monitor, 1);
    EXPECT_EQ(loaded.getOutputs()[0].monitorName, "EPSON PJ");
    EXPECT_EQ(loaded.getOutputs()[0].monitorPosition, IVec2(-1920, 0));
    EXPECT_EQ(loaded.getOutputs()[0].resolution.x, 1280);
    EXPECT_EQ(loaded.getOutputs()[0].resolution.y, 720);
    EXPECT_EQ(loaded.getOutputs()[0].rotationDegrees, 90);
}

// A malformed monitorPosition (wrong type or size) must not fail the load (QM-090).
// The output keeps its index; the name is dropped with it, since a name with the
// default (0, 0) position would match whatever display sits there.
TEST(ProjectSerializerV3Test, MalformedMonitorPositionIsSkipped) {
    const char* positions[] = {R"("left")", "[1920]", "[1920, 0, 0]", R"(["a", "b"])",
                               "[1.5, 0]", "null"};
    for (const char* position : positions) {
        SCOPED_TRACE(position);
        const std::string json = std::string(R"({"version": "3.1.0", "name": "Show",
            "scenes": [], "outputs": [{"name": "Projector 1", "monitor": 1,
            "monitorName": "EPSON PJ", "monitorPosition": )") + position + "}]}";
        ProjectSerializer serializer;
        Project project;
        ASSERT_TRUE(serializer.deserialize(json, project).isSuccess());
        ASSERT_EQ(project.getOutputs().size(), 1u);
        EXPECT_EQ(project.getOutputs()[0].monitor, 1);
        EXPECT_TRUE(project.getOutputs()[0].monitorName.empty());
        EXPECT_EQ(project.getOutputs()[0].monitorPosition, IVec2(0, 0));
    }
}

// Quad ids and group member references must survive a round-trip: previously the
// deserializer minted fresh quad ids while group members kept the saved ids, breaking
// the linkage so groups could not be re-applied on load. The persisted id is now kept.
TEST(ProjectSerializerV3Test, V3GroupMembershipRoundTrips) {
    ProjectSerializer serializer;

    Project original;
    original.setName("Grouped");

    Project::SceneData s;
    s.id = UUID::generate();
    s.name = "Stage";

    Quad q1; q1.setName("A");
    Quad q2; q2.setName("B");
    UUID q1Id = q1.getId();
    UUID q2Id = q2.getId();
    s.quads.push_back(std::move(q1)); s.texturePaths.push_back("");
    s.quads.push_back(std::move(q2)); s.texturePaths.push_back("");

    QuadGroup g(UUID::generate());
    g.setName("Pair");
    g.addMember(q1Id);
    g.addMember(q2Id);
    s.groups.push_back(g);
    original.getScenes().push_back(std::move(s));

    std::string json = serializer.serialize(original);

    Project loaded;
    ASSERT_TRUE(serializer.deserialize(json, loaded).isSuccess());

    ASSERT_EQ(loaded.getScenes().size(), 1u);
    const auto& ls = loaded.getScenes()[0];
    ASSERT_EQ(ls.quads.size(), 2u);
    ASSERT_EQ(ls.groups.size(), 1u);

    // Quad ids preserved.
    EXPECT_EQ(ls.quads[0].getId(), q1Id);
    EXPECT_EQ(ls.quads[1].getId(), q2Id);

    // Group members still point at the (preserved) quad ids — linkage intact.
    const auto& members = ls.groups[0].getMemberQuadIds();
    ASSERT_EQ(members.size(), 2u);
    EXPECT_TRUE(ls.groups[0].hasMember(q1Id));
    EXPECT_TRUE(ls.groups[0].hasMember(q2Id));
    EXPECT_EQ(ls.groups[0].getName(), "Pair");
}

// ADR-040 P1: a non-identity per-quad crop round-trips, and identity crop is not
// written at all (pre-crop files / untouched quads stay byte-identical).
TEST(ProjectSerializerExtraTest, CropRectRoundTrips) {
    ProjectSerializer serializer;

    Project original;
    Quad cropped; cropped.setName("Strip");
    cropped.getTexture().cropRect = NormalizedRect{0.25f, 0.0f, 0.25f, 1.0f};  // 2nd column
    Quad full; full.setName("Full");  // identity crop
    original.getQuads().push_back(std::move(cropped));
    original.getTexturePaths().push_back("");
    original.getQuads().push_back(std::move(full));
    original.getTexturePaths().push_back("");

    std::string json = serializer.serialize(original);

    Project loaded;
    ASSERT_TRUE(serializer.deserialize(json, loaded).isSuccess());
    ASSERT_EQ(loaded.getScenes().size(), 1u);
    const auto& quads = loaded.getScenes()[0].quads;
    ASSERT_EQ(quads.size(), 2u);

    // Cropped quad's sub-rect survives.
    const NormalizedRect& c = quads[0].getTexture().cropRect;
    EXPECT_FLOAT_EQ(c.x, 0.25f);
    EXPECT_FLOAT_EQ(c.y, 0.0f);
    EXPECT_FLOAT_EQ(c.width, 0.25f);
    EXPECT_FLOAT_EQ(c.height, 1.0f);

    // Identity-crop quad stays identity and writes no "crop" key.
    EXPECT_TRUE(quads[1].getTexture().isCropIdentity());
    EXPECT_NE(json.find("\"crop\""), std::string::npos);          // the cropped one wrote it
    EXPECT_EQ(json.find("\"crop\""), json.rfind("\"crop\""));     // ...exactly once
}

// An older file with no crop field must load as identity (full frame).
TEST(ProjectSerializerExtraTest, MissingCropLoadsAsFullFrame) {
    ProjectSerializer serializer;
    Project project;
    std::string json = R"({
        "version": "1.2.0", "name": "Old",
        "quads": [{ "name": "Q", "geometry": {
            "corners": [[0,0],[1,0],[1,1],[0,1]], "meshResolution": 2, "meshMode": 0 } }]
    })";
    ASSERT_TRUE(serializer.deserialize(json, project).isSuccess());
    ASSERT_EQ(project.getScenes().size(), 1u);
    ASSERT_EQ(project.getScenes()[0].quads.size(), 1u);
    EXPECT_TRUE(project.getScenes()[0].quads[0].getTexture().isCropIdentity());
}

// A flat-only Project (no scene DTOs) must still write the legacy v1.2 format so
// existing files and tooling are byte-compatible with before P5.
TEST(ProjectSerializerV3Test, FlatProjectStillWritesV1) {
    ProjectSerializer serializer;
    Project p;
    p.setName("FlatOnly");
    Quad q; q.setName("Q");
    p.getQuads().push_back(std::move(q));
    p.getTexturePaths().push_back("");

    std::string json = serializer.serialize(p);
    EXPECT_NE(json.find("\"version\": \"1.2.0\""), std::string::npos);
    EXPECT_EQ(json.find("\"scenes\""), std::string::npos);
}

} // namespace test
} // namespace quadmapper
