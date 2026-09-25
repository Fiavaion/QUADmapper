// Show-safe persistence (ADR-046): .qmap v3.1 sources table, input hardening, and
// every historical format still loading.

#include <gtest/gtest.h>
#include "project/ProjectSerializer.hpp"
#include "project/Project.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadGeometry.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace quadmapper {
namespace test {

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "x";
}

// One scene holding one quad bound to `source` (with an optional texturePath).
Project projectWithBoundQuad(const Project::SourceData& source, const std::string& texturePath) {
    Project project;
    project.setName("Show");
    Project::SceneData scene;
    scene.id = UUID::generate();
    scene.name = "Scene 1";
    Quad quad;
    quad.setName("Bound");
    quad.getTexture().mediaId = source.id;
    scene.quads.push_back(quad);
    scene.texturePaths.push_back(texturePath);
    project.getScenes().push_back(std::move(scene));
    project.getSources().push_back(source);
    return project;
}

// A v3.1 document with one scene whose single quad is `quadJson`.
std::string v31WithQuad(const std::string& quadJson) {
    return R"({"version": "3.1.0", "name": "T", "scenes": [{"name": "S", "quads": [)" +
           quadJson + "]}]}";
}

}  // namespace

class ProjectPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("qm_proj_" + UUID::generate().toString());
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_;
};

// ---- v3.1 sources table -------------------------------------------------------

TEST_F(ProjectPersistenceTest, WriterEmitsV31WithSourcesTable) {
    ProjectSerializer serializer;
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "procedural";
    src.proceduralType = "Noise";

    const auto json = nlohmann::json::parse(serializer.serialize(projectWithBoundQuad(src, "")));

    EXPECT_EQ(json["version"], "3.1.0");
    ASSERT_TRUE(json["sources"].is_array());
    ASSERT_EQ(json["sources"].size(), 1u);
    EXPECT_EQ(json["sources"][0]["kind"], "procedural");
    EXPECT_EQ(json["scenes"][0]["quads"][0]["textureBinding"]["mediaId"], src.id.toString());
}

TEST_F(ProjectPersistenceTest, ProceduralBindingRoundTrips) {
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "procedural";
    src.proceduralType = "Plasma";
    src.width = 640;
    src.height = 360;
    const fs::path file = dir_ / "show.qmap";

    ASSERT_TRUE(ProjectSerializer::save(projectWithBoundQuad(src, ""), file).isSuccess());
    auto loaded = ProjectSerializer::load(file);

    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const Project& p = loaded.value();
    ASSERT_EQ(p.getSources().size(), 1u);
    EXPECT_EQ(p.getSources()[0].kind, "procedural");
    EXPECT_EQ(p.getSources()[0].proceduralType, "Plasma");
    EXPECT_EQ(p.getSources()[0].width, 640);
    EXPECT_EQ(p.getSources()[0].height, 360);
    ASSERT_EQ(p.getScenes()[0].quads.size(), 1u);
    EXPECT_EQ(p.getScenes()[0].quads[0].getTexture().mediaId, src.id);  // still bound
}

TEST_F(ProjectPersistenceTest, ImageSourceWritesRelativePathAndResolvesOnLoad) {
    const fs::path image = dir_ / "media" / "backdrop.png";
    touch(image);
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "image";
    src.path = image.string();
    const fs::path file = dir_ / "show.qmap";

    ASSERT_TRUE(ProjectSerializer::save(projectWithBoundQuad(src, image.string()), file).isSuccess());

    const auto json = nlohmann::json::parse(slurp(file));
    EXPECT_EQ(json["sources"][0]["path"], "media/backdrop.png");
    // texturePath stays absolute so a v3.0 build still finds the file.
    EXPECT_EQ(json["scenes"][0]["quads"][0]["texturePath"], image.string());

    auto loaded = ProjectSerializer::load(file);
    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const Project& p = loaded.value();
    ASSERT_EQ(p.getSources().size(), 1u);
    EXPECT_TRUE(fs::equivalent(p.getSources()[0].path, image));
    EXPECT_EQ(p.getScenes()[0].quads[0].getTexture().mediaId, src.id);
}

TEST_F(ProjectPersistenceTest, MovedShowFolderResolvesRelativeMedia) {
    const fs::path show = dir_ / "show";
    touch(show / "media" / "clip.mp4");
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "video";
    src.path = (show / "media" / "clip.mp4").string();
    ASSERT_TRUE(ProjectSerializer::save(projectWithBoundQuad(src, src.path),
                                        show / "show.qmap").isSuccess());

    const fs::path moved = dir_ / "moved";
    fs::rename(show, moved);
    auto loaded = ProjectSerializer::load(moved / "show.qmap");

    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const Project& p = loaded.value();
    EXPECT_TRUE(fs::equivalent(p.getSources()[0].path, moved / "media" / "clip.mp4"));
    EXPECT_EQ(p.getScenes()[0].quads[0].getTexture().mediaId, src.id);
}

TEST_F(ProjectPersistenceTest, MissingRelativeMediaFallsBackToTexturePath) {
    const std::string absolute = (dir_ / "elsewhere" / "backdrop.png").string();
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "image";
    src.path = (dir_ / "media" / "backdrop.png").string();  // never created
    const fs::path file = dir_ / "show.qmap";
    ASSERT_TRUE(ProjectSerializer::save(projectWithBoundQuad(src, absolute), file).isSuccess());

    auto loaded = ProjectSerializer::load(file);

    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const auto& scene = loaded.value().getScenes()[0];
    EXPECT_TRUE(scene.quads[0].getTexture().mediaId.isNull());  // unresolvable source
    EXPECT_EQ(scene.texturePaths[0], absolute);                // absolute fallback kept
}

TEST_F(ProjectPersistenceTest, V31WithoutSourcesLoadsThroughTexturePath) {
    ProjectSerializer serializer;
    Project project;
    const std::string quad = R"({"name": "Q", "texturePath": "C:/Shows/a.png",
        "textureBinding": {"mediaId": "4a1b2c3d-0000-4000-8000-0000000000f9"}})";

    auto result = serializer.deserialize(v31WithQuad(quad), project);

    ASSERT_TRUE(result.isSuccess()) << result.error();
    const auto& scene = project.getScenes()[0];
    ASSERT_EQ(scene.quads.size(), 1u);
    EXPECT_TRUE(scene.quads[0].getTexture().mediaId.isNull());
    EXPECT_EQ(scene.texturePaths[0], "C:/Shows/a.png");
}

TEST_F(ProjectPersistenceTest, VideoTransportRoundTrips) {
    const fs::path clip = dir_ / "clip.mp4";
    touch(clip);
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "hap";
    src.path = clip.string();
    src.transport = Project::TransportData{true, LoopMode::PING_PONG, 2.0f};
    const fs::path file = dir_ / "show.qmap";

    ASSERT_TRUE(ProjectSerializer::save(projectWithBoundQuad(src, clip.string()), file).isSuccess());
    auto loaded = ProjectSerializer::load(file);

    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const auto& s = loaded.value().getSources()[0];
    EXPECT_EQ(s.kind, "hap");
    ASSERT_TRUE(s.transport.has_value());
    EXPECT_TRUE(s.transport->playing);
    EXPECT_EQ(s.transport->loopMode, LoopMode::PING_PONG);
    EXPECT_FLOAT_EQ(s.transport->speed, 2.0f);
}

TEST_F(ProjectPersistenceTest, CameraSourceRoundTrips) {
    ProjectSerializer serializer;
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "camera";
    src.deviceName = "USB Capture";
    Project loaded;

    ASSERT_TRUE(serializer.deserialize(serializer.serialize(projectWithBoundQuad(src, "")),
                                       loaded).isSuccess());

    ASSERT_EQ(loaded.getSources().size(), 1u);
    EXPECT_EQ(loaded.getSources()[0].deviceName, "USB Capture");
    EXPECT_EQ(loaded.getScenes()[0].quads[0].getTexture().mediaId, src.id);
}

TEST_F(ProjectPersistenceTest, UnknownSourceKindIsSkipped) {
    ProjectSerializer serializer;
    Project project;
    const std::string json = R"({"version": "3.1.0", "name": "T", "scenes": [],
        "sources": [{"id": "4a1b2c3d-0000-4000-8000-0000000000f1", "kind": "ndi"},
                    {"id": "bad", "kind": "procedural", "proceduralType": "Noise"},
                    {"id": "4a1b2c3d-0000-4000-8000-0000000000f2", "kind": "image"}]})";

    ASSERT_TRUE(serializer.deserialize(json, project).isSuccess());
    EXPECT_TRUE(project.getSources().empty());
}

// ---- Input hardening (ADR-046 §3) -----------------------------------------------

TEST_F(ProjectPersistenceTest, MalformedVec2ReturnsErr) {
    ProjectSerializer serializer;
    for (const char* corners : {
             R"([[0, 0], [1], [1, 1], [0, 1]])",           // short vec
             R"([[0, 0], [1, 0, 7], [1, 1], [0, 1]])",     // long vec
             R"([[0, 0], ["a", 0], [1, 1], [0, 1]])",      // wrong type
             R"([[0, 0], 5, [1, 1], [0, 1]])"}) {          // not an array
        Project project;
        const std::string quad = std::string(R"({"geometry": {"corners": )") + corners + "}}";
        EXPECT_TRUE(serializer.deserialize(v31WithQuad(quad), project).isError()) << corners;
    }
    Project project;
    EXPECT_TRUE(serializer.deserialize(
        v31WithQuad(R"({"colorCorrection": {"rgbGain": [1, 1]}})"), project).isError());
}

TEST_F(ProjectPersistenceTest, BadUuidsOnSceneGroupOutputAreTolerated) {
    ProjectSerializer serializer;
    Project project;
    const std::string json = R"({"version": "3.0.0", "name": "T",
        "scenes": [{"id": "zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz", "name": "S",
                    "assignedOutputId": 42,
                    "quads": [{"id": "0a1b2c3d-0000-4000-8000-000000000001"}],
                    "groups": [{"id": "not-a-uuid", "name": "G",
                                "members": ["bad", 7, "0a1b2c3d-0000-4000-8000-000000000001"],
                                "parentGroup": "also-bad"}]}],
        "outputs": [{"id": "xyz", "name": "P"}]})";

    auto result = serializer.deserialize(json, project);

    ASSERT_TRUE(result.isSuccess()) << result.error();
    const auto& scene = project.getScenes()[0];
    EXPECT_FALSE(scene.id.isNull());
    EXPECT_TRUE(scene.assignedOutputId.isNull());
    ASSERT_EQ(scene.groups.size(), 1u);
    EXPECT_FALSE(scene.groups[0].getId().isNull());
    EXPECT_EQ(scene.groups[0].getMemberQuadIds().size(), 1u);  // only the valid member
    EXPECT_FALSE(scene.groups[0].getParentGroup().has_value());
    ASSERT_EQ(project.getOutputs().size(), 1u);
    EXPECT_FALSE(project.getOutputs()[0].id.isNull());
}

TEST_F(ProjectPersistenceTest, GridCoordsAreSortedDedupedAndNanRejected) {
    ProjectSerializer serializer;
    Project project;
    // null is how nlohmann writes a NaN; 1.2 is out of range; 0.5 is duplicated.
    const std::string quad = R"({"geometry": {"uCoords": [0, 0.5, 0.5, null, 1.2, 0.25],
                                              "vCoords": [1, 0]}})";

    ASSERT_TRUE(serializer.deserialize(v31WithQuad(quad), project).isSuccess());

    const auto& geo = project.getScenes()[0].quads[0].getGeometry();
    EXPECT_EQ(geo.getUCoords(), (std::vector<float>{0.0f, 0.25f, 0.5f, 1.0f}));
    EXPECT_EQ(geo.getVCoords(), (std::vector<float>{0.0f, 1.0f}));
}

TEST_F(ProjectPersistenceTest, TooManyGridCoordsReturnsErr) {
    ProjectSerializer serializer;
    Project project;
    std::string coords = "[";
    for (int i = 0; i <= MAX_MESH_RESOLUTION + 1; ++i) {
        coords += (i ? "," : "") + std::to_string(static_cast<float>(i) / (MAX_MESH_RESOLUTION + 2));
    }
    coords += "]";
    const std::string quad = R"({"geometry": {"uCoords": )" + coords + R"(, "vCoords": [0, 1]}})";

    EXPECT_TRUE(serializer.deserialize(v31WithQuad(quad), project).isError());
}

TEST_F(ProjectPersistenceTest, OutOfRangeMeshModeFallsBackToCorners) {
    ProjectSerializer serializer;
    for (const char* mode : {"7", "-3"}) {
        Project project;
        const std::string quad = std::string(R"({"geometry": {"meshMode": )") + mode + "}}";
        ASSERT_TRUE(serializer.deserialize(v31WithQuad(quad), project).isSuccess()) << mode;
        EXPECT_EQ(project.getScenes()[0].quads[0].getGeometry().getMeshMode(),
                  MeshMode::CORNERS_ONLY) << mode;
    }
}

TEST_F(ProjectPersistenceTest, OutputMonitorResolutionRotationAreClamped) {
    ProjectSerializer serializer;
    Project project;
    const std::string json = R"({"version": "3.0.0", "name": "T", "scenes": [],
        "outputs": [
          {"id": "a", "monitor": 9999, "resolution": [0, 999999], "rotation": 1000},
          {"id": "b", "monitor": -50, "resolution": [1280, 720], "rotation": -90}]})";

    ASSERT_TRUE(serializer.deserialize(json, project).isSuccess());

    const auto& outs = project.getOutputs();
    ASSERT_EQ(outs.size(), 2u);
    EXPECT_EQ(outs[0].monitor, 63);
    EXPECT_EQ(outs[0].resolution.x, 1);
    EXPECT_EQ(outs[0].resolution.y, 16384);
    EXPECT_EQ(outs[0].rotationDegrees, 270);  // 1000 % 360 = 280 -> nearest quarter turn
    EXPECT_EQ(outs[1].monitor, -1);
    EXPECT_EQ(outs[1].rotationDegrees, 270);  // -90 normalizes to 270
    EXPECT_TRUE(serializer.deserialize(R"({"version": "3.0.0", "scenes": [],
        "outputs": [{"resolution": [1920]}]})", project).isError());
}

// ---- Historical fixtures (ADR-032 / ADR-046) -------------------------------------

TEST(ProjectFixtureTest, EveryHistoricalFormatLoads) {
    const fs::path dir = QM_PROJECT_FIXTURES_DIR;
    ASSERT_TRUE(fs::is_directory(dir)) << dir.string();

    struct Expect { const char* file; size_t scenes; size_t quads; };
    const Expect expected[] = {
        {"v1.0-flat.qmap", 1, 2},
        {"v1.2-flat-grid.qmap", 1, 1},
        {"v2.0-scenes.qmap", 2, 1},
        {"v3.0-scenes-outputs.qmap", 1, 1},
        {"v3.1-sources.qmap", 1, 2},
    };
    for (const auto& e : expected) {
        auto loaded = ProjectSerializer::load(dir / e.file);
        ASSERT_TRUE(loaded.isSuccess()) << e.file << ": " << loaded.error().message;
        EXPECT_EQ(loaded.value().getScenes().size(), e.scenes) << e.file;
        EXPECT_EQ(loaded.value().getQuads().size(), e.quads) << e.file;
    }

    size_t onDisk = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".qmap") ++onDisk;
    }
    EXPECT_EQ(onDisk, std::size(expected)) << "a fixture was added without a load check";
}

TEST(ProjectFixtureTest, V31FixtureKeepsProceduralAndFallsBackForMissingVideo) {
    auto loaded = ProjectSerializer::load(fs::path(QM_PROJECT_FIXTURES_DIR) / "v3.1-sources.qmap");
    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const auto& quads = loaded.value().getScenes()[0].quads;
    ASSERT_EQ(quads.size(), 2u);
    EXPECT_EQ(quads[0].getTexture().mediaId.toString(), "4a1b2c3d-0000-4000-8000-0000000000f1");
    EXPECT_TRUE(quads[1].getTexture().mediaId.isNull());  // video not shipped -> texturePath
    EXPECT_EQ(loaded.value().getScenes()[0].texturePaths[1], "C:/Shows/media/not-shipped.mp4");
}

TEST(ProjectFixtureTest, V12FixtureKeepsGridAndOffsets) {
    auto loaded = ProjectSerializer::load(fs::path(QM_PROJECT_FIXTURES_DIR) / "v1.2-flat-grid.qmap");
    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const auto& geo = loaded.value().getScenes()[0].quads[0].getGeometry();
    EXPECT_EQ(geo.getUCoords(), (std::vector<float>{0.0f, 0.3f, 1.0f}));
    EXPECT_EQ(geo.getMeshMode(), MeshMode::MESH_EDIT);
    EXPECT_TRUE(geo.hasMeshModifications());
}

}  // namespace test
}  // namespace quadmapper
