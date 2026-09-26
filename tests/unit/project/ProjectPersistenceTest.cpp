// Show-safe persistence (ADR-046): .qmap v3.1 sources table, input hardening, and
// every historical format still loading.

#include <gtest/gtest.h>
#include "project/MediaRelocation.hpp"
#include "project/ProjectSerializer.hpp"
#include "project/Project.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadGeometry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
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

TEST_F(ProjectPersistenceTest, WriterEmitsV32WithSourcesTable) {
    ProjectSerializer serializer;
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "procedural";
    src.proceduralType = "Noise";

    const auto json = nlohmann::json::parse(serializer.serialize(projectWithBoundQuad(src, "")));

    EXPECT_EQ(json["version"], "3.2.0");
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

// ---- Missing media (PRD §4.1.8) ---------------------------------------------------

TEST_F(ProjectPersistenceTest, MissingFileSourceKeepsReferenceAndTexturePath) {
    const std::string absolute = (dir_ / "elsewhere" / "backdrop.png").string();
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "image";
    src.path = (dir_ / "media" / "backdrop.png").string();  // never created
    const fs::path file = dir_ / "show.qmap";
    ASSERT_TRUE(ProjectSerializer::save(projectWithBoundQuad(src, absolute), file).isSuccess());

    auto loaded = ProjectSerializer::load(file);

    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const Project& p = loaded.value();
    const auto& scene = p.getScenes()[0];
    EXPECT_EQ(scene.quads[0].getTexture().mediaId, src.id);  // still bound
    EXPECT_EQ(scene.texturePaths[0], absolute);              // absolute fallback kept
    ASSERT_EQ(p.getSources().size(), 1u);
    EXPECT_TRUE(p.getSources()[0].missing);
    EXPECT_EQ(fs::path(p.getSources()[0].path), fs::path(src.path).lexically_normal());
    EXPECT_EQ(p.getQuads()[0].getTexture().mediaId, src.id);  // the flat view agrees
}

TEST_F(ProjectPersistenceTest, PresentFileSourceIsNotMissing) {
    const fs::path image = dir_ / "backdrop.png";
    touch(image);
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "image";
    src.path = image.string();
    const fs::path file = dir_ / "show.qmap";
    ASSERT_TRUE(ProjectSerializer::save(projectWithBoundQuad(src, ""), file).isSuccess());

    auto loaded = ProjectSerializer::load(file);

    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    EXPECT_FALSE(loaded.value().getSources()[0].missing);
}

TEST_F(ProjectPersistenceTest, NonFileSourcesAreNeverMissing) {
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "camera";
    src.deviceName = "Unplugged Cam";
    const fs::path file = dir_ / "show.qmap";
    ASSERT_TRUE(ProjectSerializer::save(projectWithBoundQuad(src, ""), file).isSuccess());

    auto loaded = ProjectSerializer::load(file);

    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    EXPECT_FALSE(loaded.value().getSources()[0].missing);
    EXPECT_EQ(loaded.value().getScenes()[0].quads[0].getTexture().mediaId, src.id);
}

// Loading and saving again without relocating writes every missing path back as it was.
TEST_F(ProjectPersistenceTest, MissingMediaRoundTripsTheOriginalPaths) {
    const fs::path file = dir_ / "show.qmap";
    const std::string doc = R"({"version": "3.2.0", "name": "Show",
        "sources": [
          {"id": "4a1b2c3d-0000-4000-8000-0000000000a1", "kind": "image", "path": "media/gone.png"},
          {"id": "4a1b2c3d-0000-4000-8000-0000000000a2", "kind": "video", "path": "Z:/Shows/gone.mp4",
           "transport": {"playing": false, "loopMode": "once", "speed": 2.0}}],
        "scenes": [{"id": "4a1b2c3d-0000-4000-8000-0000000000c1", "name": "S",
          "quads": [
            {"id": "4a1b2c3d-0000-4000-8000-000000000001", "name": "A",
             "textureBinding": {"mediaId": "4a1b2c3d-0000-4000-8000-0000000000a1"}},
            {"id": "4a1b2c3d-0000-4000-8000-000000000002", "name": "B",
             "texturePath": "Z:/Shows/gone.mp4",
             "textureBinding": {"mediaId": "4a1b2c3d-0000-4000-8000-0000000000a2"}}],
          "objects": [
            {"id": "4a1b2c3d-0000-4000-8000-0000000000b1", "name": "Prop",
             "path": "models/gone.obj",
             "texture": {"mediaId": "4a1b2c3d-0000-4000-8000-0000000000a1"}}]}]})";
    {
        std::ofstream(file, std::ios::binary) << doc;
    }

    auto loaded = ProjectSerializer::load(file);
    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const Project& p = loaded.value();
    ASSERT_EQ(p.getSources().size(), 2u);
    EXPECT_TRUE(p.getSources()[0].missing);
    EXPECT_TRUE(p.getSources()[1].missing);
    EXPECT_TRUE(p.getScenes()[0].objects[0].missing);
    EXPECT_EQ(p.getScenes()[0].objects[0].texture.mediaId.toString(),
              "4a1b2c3d-0000-4000-8000-0000000000a1");

    ASSERT_TRUE(ProjectSerializer::save(p, file).isSuccess());
    const auto json = nlohmann::json::parse(slurp(file));
    EXPECT_EQ(json["sources"][0]["path"], "media/gone.png");
    EXPECT_EQ(json["sources"][1]["path"], "Z:/Shows/gone.mp4");
    EXPECT_EQ(json["sources"][1]["transport"]["loopMode"], "once");
    EXPECT_FLOAT_EQ(json["sources"][1]["transport"]["speed"].get<float>(), 2.0f);
    const auto& quads = json["scenes"][0]["quads"];
    EXPECT_EQ(quads[0]["textureBinding"]["mediaId"], "4a1b2c3d-0000-4000-8000-0000000000a1");
    EXPECT_EQ(quads[1]["textureBinding"]["mediaId"], "4a1b2c3d-0000-4000-8000-0000000000a2");
    EXPECT_EQ(quads[1]["texturePath"], "Z:/Shows/gone.mp4");
    const auto& object = json["scenes"][0]["objects"][0];
    EXPECT_EQ(object["path"], "models/gone.obj");
    EXPECT_EQ(object["texture"]["mediaId"], "4a1b2c3d-0000-4000-8000-0000000000a1");
    // The flag is in memory only: the schema is unchanged.
    EXPECT_FALSE(json["sources"][0].contains("missing"));
    EXPECT_FALSE(object.contains("missing"));
}

TEST_F(ProjectPersistenceTest, ObjectWithMissingMeshKeepsPathAndMedia) {
    const fs::path present = dir_ / "models" / "here.obj";
    touch(present);
    Project project;
    Project::SceneData scene;
    scene.id = UUID::generate();
    Project::ObjectData gone;
    gone.id = UUID::generate();
    gone.path = (dir_ / "models" / "gone.obj").string();
    Project::ObjectData here;
    here.id = UUID::generate();
    here.path = present.string();
    Project::ObjectData pathless;  // no mesh path at all: nothing to relocate
    pathless.id = UUID::generate();
    scene.objects = {gone, here, pathless};
    project.getScenes().push_back(scene);
    const fs::path file = dir_ / "show.qmap";
    ASSERT_TRUE(ProjectSerializer::save(project, file).isSuccess());

    auto loaded = ProjectSerializer::load(file);

    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const auto& objects = loaded.value().getScenes()[0].objects;
    ASSERT_EQ(objects.size(), 3u);
    EXPECT_TRUE(objects[0].missing);
    EXPECT_EQ(fs::path(objects[0].path), fs::path(gone.path).lexically_normal());
    EXPECT_FALSE(objects[1].missing);
    EXPECT_FALSE(objects[2].missing);
    EXPECT_TRUE(objects[2].path.empty());
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
    src.transport = Project::TransportData{true, LoopMode::PING_PONG, 2.0f, true};
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
    EXPECT_TRUE(s.transport->timelineLocked);
}

// ADR-050 §5: timelineLocked is additive. A transport without it (every file before
// it) loads unlocked, "pingPong" loads as real Ping-Pong, and a malformed value is
// ignored rather than failing the load.
TEST_F(ProjectPersistenceTest, TimelineLockIsAdditiveAndPingPongLoads) {
    ProjectSerializer serializer;
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "video";
    src.path = (dir_ / "clip.mp4").string();
    src.transport = Project::TransportData{false, LoopMode::PING_PONG, 1.0f, true};
    auto json = nlohmann::json::parse(serializer.serialize(projectWithBoundQuad(src, src.path)));
    auto& transport = json["sources"][0]["transport"];
    EXPECT_EQ(transport["timelineLocked"], true);
    EXPECT_EQ(transport["loopMode"], "pingPong");

    transport.erase("timelineLocked");  // as an older build wrote it
    Project old;
    ASSERT_TRUE(serializer.deserialize(json.dump(), old).isSuccess());
    ASSERT_TRUE(old.getSources()[0].transport.has_value());
    EXPECT_FALSE(old.getSources()[0].transport->timelineLocked);
    EXPECT_EQ(old.getSources()[0].transport->loopMode, LoopMode::PING_PONG);

    transport["timelineLocked"] = "yes";  // wrong type
    Project odd;
    ASSERT_TRUE(serializer.deserialize(json.dump(), odd).isSuccess());
    EXPECT_FALSE(odd.getSources()[0].transport->timelineLocked);
}

// ADR-050 §5: a source's volume / mute and the show's master volume / mute round-trip.
TEST_F(ProjectPersistenceTest, AudioVolumesAndMutesRoundTrip) {
    const fs::path clip = dir_ / "clip.mp4";
    touch(clip);
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "video";
    src.path = clip.string();
    src.transport = Project::TransportData{};
    src.transport->volume = 0.35f;
    src.transport->muted = true;
    Project project = projectWithBoundQuad(src, clip.string());
    project.getAudio().masterVolume = 0.6f;
    project.getAudio().masterMuted = true;
    const fs::path file = dir_ / "loud.qmap";

    ASSERT_TRUE(ProjectSerializer::save(project, file).isSuccess());
    auto loaded = ProjectSerializer::load(file);
    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const auto& s = loaded.value().getSources()[0];
    ASSERT_TRUE(s.transport.has_value());
    EXPECT_FLOAT_EQ(s.transport->volume, 0.35f);
    EXPECT_TRUE(s.transport->muted);
    EXPECT_FLOAT_EQ(loaded.value().getAudio().masterVolume, 0.6f);
    EXPECT_TRUE(loaded.value().getAudio().masterMuted);
}

// Files written before audio carry none of the fields: full volume, unmuted, so an old
// show plays exactly as it did, now with its sound. Malformed values are ignored (and
// out-of-range ones clamped) rather than failing the load.
TEST_F(ProjectPersistenceTest, AudioFieldsAreAdditive) {
    ProjectSerializer serializer;
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "video";
    src.path = (dir_ / "clip.mp4").string();
    src.transport = Project::TransportData{};
    auto json = nlohmann::json::parse(serializer.serialize(projectWithBoundQuad(src, src.path)));
    auto& transport = json["sources"][0]["transport"];
    EXPECT_FLOAT_EQ(transport["volume"].get<float>(), 1.0f);
    EXPECT_EQ(transport["muted"], false);
    EXPECT_FLOAT_EQ(json["audio"]["masterVolume"].get<float>(), 1.0f);
    EXPECT_EQ(json["audio"]["masterMuted"], false);

    transport.erase("volume");  // as an older build wrote it
    transport.erase("muted");
    json.erase("audio");
    Project old;
    ASSERT_TRUE(serializer.deserialize(json.dump(), old).isSuccess());
    EXPECT_FLOAT_EQ(old.getSources()[0].transport->volume, 1.0f);
    EXPECT_FALSE(old.getSources()[0].transport->muted);
    EXPECT_FLOAT_EQ(old.getAudio().masterVolume, 1.0f);
    EXPECT_FALSE(old.getAudio().masterMuted);

    transport["volume"] = 4.0f;
    transport["muted"] = "yes";
    json["audio"] = {{"masterVolume", -3.0}, {"masterMuted", 1}};
    Project odd;
    ASSERT_TRUE(serializer.deserialize(json.dump(), odd).isSuccess());
    EXPECT_FLOAT_EQ(odd.getSources()[0].transport->volume, 1.0f);
    EXPECT_FALSE(odd.getSources()[0].transport->muted);
    EXPECT_FLOAT_EQ(odd.getAudio().masterVolume, 0.0f);
    EXPECT_FALSE(odd.getAudio().masterMuted);

    json["audio"] = "loud";  // not an object
    Project wrong;
    ASSERT_TRUE(serializer.deserialize(json.dump(), wrong).isSuccess());
    EXPECT_FLOAT_EQ(wrong.getAudio().masterVolume, 1.0f);
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

// ---- 3D objects and projectors (.qmap v3.2, ADR-052) ------------------------------

TEST_F(ProjectPersistenceTest, ObjectWithTransformAndMediaRoundTrips) {
    const fs::path model = dir_ / "models" / "column.obj";
    touch(model);
    Project::SourceData src;
    src.id = UUID::generate();
    src.kind = "procedural";
    src.proceduralType = "Plasma";
    Project project = projectWithBoundQuad(src, "");
    Project::ObjectData object;
    object.id = UUID::generate();
    object.name = "Column";
    object.path = model.string();
    object.transform = glm::mat4(1.0f);
    object.transform[0][0] = 2.0f;
    object.transform[3] = glm::vec4(0.5f, -1.0f, 3.0f, 1.0f);
    object.texture.mediaId = src.id;
    object.texture.flipV = true;
    object.visible = false;
    object.opacity = 0.25f;
    project.getScenes()[0].objects.push_back(object);
    const fs::path file = dir_ / "show.qmap";

    ASSERT_TRUE(ProjectSerializer::save(project, file).isSuccess());
    const auto json = nlohmann::json::parse(slurp(file));
    EXPECT_EQ(json["scenes"][0]["objects"][0]["path"], "models/column.obj");  // relative

    auto loaded = ProjectSerializer::load(file);
    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const auto& objects = loaded.value().getScenes()[0].objects;
    ASSERT_EQ(objects.size(), 1u);
    const Project::ObjectData& o = objects[0];
    EXPECT_EQ(o.id, object.id);
    EXPECT_EQ(o.name, "Column");
    EXPECT_TRUE(fs::equivalent(o.path, model));
    EXPECT_EQ(o.transform, object.transform);
    EXPECT_EQ(o.texture.mediaId, src.id);
    EXPECT_TRUE(o.texture.flipV);
    EXPECT_FALSE(o.visible);
    EXPECT_FLOAT_EQ(o.opacity, 0.25f);
}

TEST_F(ProjectPersistenceTest, ObjectMediaNotInSourcesIsDropped) {
    ProjectSerializer serializer;
    Project project;
    Project::SceneData scene;
    scene.id = UUID::generate();
    Project::ObjectData object;
    object.path = "C:/Shows/prop.obj";
    object.texture.mediaId = UUID::generate();  // no sources entry
    scene.objects.push_back(object);
    project.getScenes().push_back(scene);

    Project loaded;
    ASSERT_TRUE(serializer.deserialize(serializer.serialize(project), loaded).isSuccess());

    ASSERT_EQ(loaded.getScenes()[0].objects.size(), 1u);
    EXPECT_TRUE(loaded.getScenes()[0].objects[0].texture.mediaId.isNull());
    EXPECT_EQ(loaded.getScenes()[0].objects[0].path, "C:/Shows/prop.obj");
}

TEST_F(ProjectPersistenceTest, ProjectorWithCalibrationRoundTrips) {
    ProjectSerializer serializer;
    Project project;
    Project::SceneData scene;
    scene.id = UUID::generate();
    project.getScenes().push_back(scene);
    Project::OutputData output;
    output.id = UUID::generate();
    output.name = "Projector 1";
    ProjectorCamera& cam = output.projector;
    cam.intrinsics = ProjectorIntrinsics::fromThrowRatio(1.2f, 1280, 800, 0.25f);
    cam.intrinsics.fy = 1530.0f;
    const float c = std::cos(0.3f), s = std::sin(0.3f);
    cam.pose.rotation = glm::mat3(glm::vec3(c, 0, -s), glm::vec3(0, 1, 0), glm::vec3(s, 0, c));
    cam.pose.translation = Vec3(0.1f, -0.2f, 4.5f);
    cam.calibrated = true;
    output.calibrationPoints = {{Vec3(0, 0, 0), Vec2(640, 400), true},
                                {Vec3(1, 2, 3), Vec2(10.5f, 20.25f), false}};
    project.getOutputs().push_back(output);

    Project loaded;
    ASSERT_TRUE(serializer.deserialize(serializer.serialize(project), loaded).isSuccess());

    ASSERT_EQ(loaded.getOutputs().size(), 1u);
    const Project::OutputData& o = loaded.getOutputs()[0];
    EXPECT_EQ(o.projector.intrinsics.width, 1280);
    EXPECT_EQ(o.projector.intrinsics.height, 800);
    EXPECT_FLOAT_EQ(o.projector.intrinsics.fx, cam.intrinsics.fx);
    EXPECT_FLOAT_EQ(o.projector.intrinsics.fy, 1530.0f);
    EXPECT_FLOAT_EQ(o.projector.intrinsics.cy, cam.intrinsics.cy);
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            EXPECT_FLOAT_EQ(o.projector.pose.rotation[col][row], cam.pose.rotation[col][row]);
        }
    }
    EXPECT_EQ(o.projector.pose.translation, cam.pose.translation);
    EXPECT_TRUE(o.projector.calibrated);
    ASSERT_EQ(o.calibrationPoints.size(), 2u);
    EXPECT_EQ(o.calibrationPoints[1].world, Vec3(1, 2, 3));
    EXPECT_EQ(o.calibrationPoints[1].pixel, Vec2(10.5f, 20.25f));
    EXPECT_FALSE(o.calibrationPoints[1].enabled);
    EXPECT_TRUE(o.calibrationPoints[0].enabled);
}

TEST_F(ProjectPersistenceTest, InvalidProjectorFallsBackToDefaultLens) {
    ProjectSerializer serializer;
    Project project;
    const std::string json = R"({"version": "3.2.0", "scenes": [], "outputs": [
        {"id": "4a1b2c3d-0000-4000-8000-0000000000e1", "resolution": [1280, 720],
         "projector": {"intrinsics": {"fx": -5, "fy": 100, "cx": 1, "cy": 1,
                                      "width": 1280, "height": 720},
                       "pose": {"rotation": [2, 0, 0, 0, 2, 0, 0, 0, 2], "translation": [0, 0, 1]},
                       "calibrated": true}}]})";

    ASSERT_TRUE(serializer.deserialize(json, project).isSuccess());

    const ProjectorCamera& cam = project.getOutputs()[0].projector;
    EXPECT_FALSE(cam.calibrated);
    EXPECT_FLOAT_EQ(cam.intrinsics.fx, 1280.0f * 1.5f);
    EXPECT_EQ(cam.intrinsics.height, 720);
}

TEST_F(ProjectPersistenceTest, MalformedObjectOrProjectorReturnsErr) {
    ProjectSerializer serializer;
    const char* bad[] = {
        R"({"version": "3.2.0", "scenes": [{"objects": {"id": 1}}]})",
        R"({"version": "3.2.0", "scenes": [{"objects": [{"transform": [1, 0, 0]}]}]})",
        R"({"version": "3.2.0", "scenes": [{"objects": [{"opacity": "high"}]}]})",
        R"({"version": "3.2.0", "scenes": [], "outputs": [{"projector": {"pose": {"rotation": [1, 0]}}}]})",
        R"({"version": "3.2.0", "scenes": [], "outputs": [{"projector": {"intrinsics": {"fx": "x"}}}]})",
        R"({"version": "3.2.0", "scenes": [], "outputs": [{"projector": {"points": [{"world": [0, 0]}]}}]})",
    };
    for (const char* json : bad) {
        Project project;
        EXPECT_TRUE(serializer.deserialize(json, project).isError()) << json;
    }
}

TEST_F(ProjectPersistenceTest, OlderOutputsGetAnUncalibratedDefaultProjector) {
    ProjectSerializer serializer;
    Project project;
    ASSERT_TRUE(serializer.deserialize(R"({"version": "3.1.0", "scenes": [],
        "outputs": [{"id": "4a1b2c3d-0000-4000-8000-0000000000e1", "resolution": [1280, 720]}]})",
        project).isSuccess());

    const Project::OutputData& o = project.getOutputs()[0];
    EXPECT_FALSE(o.projector.calibrated);
    EXPECT_EQ(o.projector.intrinsics.width, 1280);
    EXPECT_FLOAT_EQ(o.projector.intrinsics.cx, 640.0f);
    EXPECT_TRUE(o.calibrationPoints.empty());
}

// ---- Relocating missing media (PRD §4.1.8, MediaRelocation.hpp) -------------------

TEST_F(ProjectPersistenceTest, FolderSearchFindsSameNameDirectlyAndRecursively) {
    touch(dir_ / "clip.mp4");
    touch(dir_ / "a" / "b" / "deep.png");
    touch(dir_ / "a" / "other.png");

    const relocate::FolderIndex index = relocate::indexFolder(dir_);

    EXPECT_FALSE(index.truncated);
    EXPECT_TRUE(fs::equivalent(relocate::findInIndex(index, "C:/old/show/clip.mp4"), dir_ / "clip.mp4"));
    EXPECT_TRUE(fs::equivalent(relocate::findInIndex(index, "D:/elsewhere/deep.png"),
                               dir_ / "a" / "b" / "deep.png"));
    EXPECT_TRUE(relocate::findInIndex(index, "C:/old/absent.png").empty());
    // The name must match whole: a different extension is a different file.
    EXPECT_TRUE(relocate::findInIndex(index, "C:/old/clip.mov").empty());
}

TEST_F(ProjectPersistenceTest, FolderSearchIgnoresFoldersNamedLikeTheFile) {
    fs::create_directories(dir_ / "clip.mp4");  // a folder, not a file
    const relocate::FolderIndex index = relocate::indexFolder(dir_);
    EXPECT_TRUE(relocate::findInIndex(index, "C:/old/clip.mp4").empty());
}

TEST_F(ProjectPersistenceTest, FolderSearchFirstMatchIsShallowestThenSmallestPath) {
    touch(dir_ / "z" / "y" / "logo.png");  // deeper: loses to any shallower copy
    touch(dir_ / "b" / "logo.png");
    touch(dir_ / "a" / "logo.png");        // same depth as b: smaller path wins
    touch(dir_ / "m" / "n" / "title.png");
    touch(dir_ / "title.png");             // directly in the folder: always wins

    const relocate::FolderIndex index = relocate::indexFolder(dir_);

    EXPECT_TRUE(fs::equivalent(relocate::findInIndex(index, "logo.png"), dir_ / "a" / "logo.png"));
    EXPECT_TRUE(fs::equivalent(relocate::findInIndex(index, "title.png"), dir_ / "title.png"));
}

#ifdef _WIN32
TEST_F(ProjectPersistenceTest, FolderSearchIsCaseInsensitiveOnWindows) {
    touch(dir_ / "Media" / "Intro Clip.MP4");
    const relocate::FolderIndex index = relocate::indexFolder(dir_);
    const fs::path found = relocate::findInIndex(index, "C:/Old/intro clip.mp4");
    ASSERT_FALSE(found.empty());
    EXPECT_EQ(found.filename().string(), "Intro Clip.MP4");  // the file's real name
    EXPECT_EQ(relocate::nameKey(L"ABC.Png"), relocate::nameKey(L"abc.pNG"));
}
#endif

TEST_F(ProjectPersistenceTest, FolderSearchStopsAtTheCapOrWhenCancelled) {
    for (int i = 0; i < 5; ++i) touch(dir_ / ("f" + std::to_string(i) + ".png"));

    const relocate::FolderIndex capped = relocate::indexFolder(dir_, nullptr, 2);
    EXPECT_TRUE(capped.truncated);
    EXPECT_LE(capped.byName.size(), 2u);

    std::atomic<bool> cancel{true};
    const relocate::FolderIndex cancelled = relocate::indexFolder(dir_, &cancel);
    EXPECT_TRUE(cancelled.truncated);
    EXPECT_TRUE(cancelled.byName.empty());
}

TEST_F(ProjectPersistenceTest, FolderSearchOfAMissingFolderFindsNothing) {
    const relocate::FolderIndex index = relocate::indexFolder(dir_ / "no-such-folder");
    EXPECT_TRUE(index.byName.empty());
    EXPECT_TRUE(relocate::findInIndex(index, "clip.mp4").empty());
}

// Relocating one file resolves every other missing file sitting in its new folder.
TEST_F(ProjectPersistenceTest, SiblingsInTheNewFolderAreFound) {
    const fs::path moved = dir_ / "moved";
    touch(moved / "a.png");
    touch(moved / "b.mp4");
    fs::create_directories(moved / "c.png");  // a folder with the name does not count

    EXPECT_TRUE(fs::equivalent(relocate::findSibling(moved, "C:/old/show/b.mp4"), moved / "b.mp4"));
    EXPECT_TRUE(relocate::findSibling(moved, "C:/old/show/c.png").empty());
    EXPECT_TRUE(relocate::findSibling(moved, "C:/old/show/elsewhere.png").empty());
    EXPECT_TRUE(relocate::findSibling(fs::path(), "C:/old/show/a.png").empty());
    EXPECT_TRUE(relocate::findSibling(moved, fs::path()).empty());
#ifdef _WIN32
    EXPECT_FALSE(relocate::findSibling(moved, "C:/old/show/A.PNG").empty());
#endif
}

// ---- Scene order (reorder, US-008 numbering) -------------------------------------

TEST_F(ProjectPersistenceTest, SceneOrderRoundTrips) {
    Project project;
    for (const char* name : {"Intro", "Main", "Finale"}) {
        Project::SceneData scene;
        scene.id = UUID::generate();
        scene.name = name;
        project.getScenes().push_back(std::move(scene));
    }
    // Reorder as the Scenes panel does: Finale to the top.
    auto& scenes = project.getScenes();
    std::rotate(scenes.begin(), scenes.begin() + 2, scenes.end());
    const std::vector<UUID> ids{scenes[0].id, scenes[1].id, scenes[2].id};
    const fs::path file = dir_ / "order.qmap";

    ASSERT_TRUE(ProjectSerializer::save(project, file).isSuccess());
    auto loaded = ProjectSerializer::load(file);

    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const auto& back = loaded.value().getScenes();
    ASSERT_EQ(back.size(), 3u);
    EXPECT_EQ(back[0].name, "Finale");
    EXPECT_EQ(back[1].name, "Intro");
    EXPECT_EQ(back[2].name, "Main");
    for (size_t i = 0; i < 3; ++i) EXPECT_EQ(back[i].id, ids[i]);
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
        {"v3.2-objects-projector.qmap", 1, 1},
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

TEST(ProjectFixtureTest, V31FixtureKeepsProceduralAndTheMissingVideo) {
    auto loaded = ProjectSerializer::load(fs::path(QM_PROJECT_FIXTURES_DIR) / "v3.1-sources.qmap");
    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const auto& quads = loaded.value().getScenes()[0].quads;
    ASSERT_EQ(quads.size(), 2u);
    EXPECT_EQ(quads[0].getTexture().mediaId.toString(), "4a1b2c3d-0000-4000-8000-0000000000f1");
    // The video is not shipped: the quad stays bound to it for relocation, and
    // texturePath is still there as the fallback.
    EXPECT_EQ(quads[1].getTexture().mediaId.toString(), "4a1b2c3d-0000-4000-8000-0000000000f2");
    EXPECT_TRUE(loaded.value().getSources()[1].missing);
    EXPECT_FALSE(loaded.value().getSources()[0].missing);
    EXPECT_EQ(loaded.value().getScenes()[0].texturePaths[1], "C:/Shows/media/not-shipped.mp4");
    for (const auto& source : loaded.value().getSources()) {  // written before timeline lock
        if (source.transport) EXPECT_FALSE(source.transport->timelineLocked);
    }
}

TEST(ProjectFixtureTest, V32FixtureKeepsObjectAndProjector) {
    const fs::path dir = QM_PROJECT_FIXTURES_DIR;
    auto loaded = ProjectSerializer::load(dir / "v3.2-objects-projector.qmap");
    ASSERT_TRUE(loaded.isSuccess()) << loaded.error().message;
    const Project& p = loaded.value();

    ASSERT_EQ(p.getScenes()[0].objects.size(), 1u);
    const Project::ObjectData& o = p.getScenes()[0].objects[0];
    EXPECT_EQ(o.name, "Column");
    // The mesh file is not shipped: the path still resolves against the project folder.
    EXPECT_EQ(fs::path(o.path), (dir / "models" / "not-shipped.obj").lexically_normal());
    EXPECT_TRUE(o.missing);
    EXPECT_FLOAT_EQ(o.transform[0][0], 2.0f);
    EXPECT_FLOAT_EQ(o.transform[3][0], 0.5f);   // translation column
    EXPECT_FLOAT_EQ(o.transform[3][2], -1.0f);
    EXPECT_EQ(o.texture.mediaId.toString(), "4a1b2c3d-0000-4000-8000-0000000000f1");
    EXPECT_TRUE(o.texture.flipH);
    EXPECT_FLOAT_EQ(o.opacity, 0.75f);

    const Project::OutputData& out = p.getOutputs()[0];
    EXPECT_TRUE(out.projector.calibrated);
    EXPECT_FLOAT_EQ(out.projector.intrinsics.cy, 600.0f);
    // On disk the rotation is row-major (OpenCV); glm is [column][row]. The fixture's
    // rotation is not symmetric, so a transposed reader fails here.
    EXPECT_FLOAT_EQ(out.projector.pose.rotation[1][0], -1.0f);  // row 0, column 1
    EXPECT_FLOAT_EQ(out.projector.pose.rotation[0][1], 1.0f);   // row 1, column 0
    EXPECT_FLOAT_EQ(out.projector.pose.translation.z, 5.0f);
    ASSERT_EQ(out.calibrationPoints.size(), 2u);
    EXPECT_FALSE(out.calibrationPoints[1].enabled);
    Vec2 pixel;
    ASSERT_TRUE(out.projector.project(Vec3(0.0f), pixel));
    EXPECT_EQ(pixel, out.calibrationPoints[0].pixel);
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
