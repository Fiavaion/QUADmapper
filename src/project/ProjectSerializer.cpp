#include "ProjectSerializer.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include <nlohmann/json.hpp>

namespace quadmapper {

// .qmap format versioning (see ADR-032, ADR-039).
//   1.x = flat single-quad-list format
//   2.x = legacy scene-based format ("scenes" array) — read-only (ADR-033)
//   3.x = multi-scene + per-output routing: "scenes" (id/name/quads/groups/view/
//         assignedOutputId) + "outputs" arrays (ADR-039). The format the app writes
//         once a project has scenes; v1/v2 are migrated in memory on read.
// Readers accept any major <= kMaxSupportedMajor; v1/v2 are wrapped/mapped into the
// scene model so every read path yields >= 1 scene.
static constexpr int kMaxSupportedMajor = 3;

// Single version policy for every read path (ADR-032). Returns the validated MAJOR version,
// or an error message: missing/malformed version rejects, and a major newer than this build
// supports rejects with an actionable message instead of silently half-parsing the file.
static Result<int, std::string> validateVersion(const nlohmann::json& json) {
    if (!json.contains("version")) {
        return Result<int, std::string>::error("Missing version field in project file");
    }
    std::string version = json["version"].get<std::string>();
    int major = 0;
    try {
        size_t dot = version.find('.');
        major = std::stoi(dot == std::string::npos ? version : version.substr(0, dot));
    } catch (const std::exception&) {
        return Result<int, std::string>::error("Malformed project version: " + version);
    }
    if (major < 1) {
        return Result<int, std::string>::error("Malformed project version: " + version);
    }
    if (major > kMaxSupportedMajor) {
        return Result<int, std::string>::error(
            "Project version " + version + " was created by a newer QUADmapper; please update.");
    }
    return Result<int, std::string>::success(major);
}

// Helper to serialize a Vec2
static nlohmann::json serializeVec2(const Vec2& v) {
    return nlohmann::json::array({v.x, v.y});
}

// Helper to deserialize a Vec2
static Vec2 deserializeVec2(const nlohmann::json& j) {
    return Vec2(j[0].get<float>(), j[1].get<float>());
}

// Helper to serialize a Vec3
static nlohmann::json serializeVec3(const Vec3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}

// Helper to deserialize a Vec3
static Vec3 deserializeVec3(const nlohmann::json& j) {
    return Vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

// Helper to serialize quad geometry
static nlohmann::json serializeQuadGeometry(const QuadGeometry& geometry) {
    nlohmann::json j;

    // Serialize corners
    const auto& corners = geometry.getCorners();
    j["corners"] = nlohmann::json::array({
        serializeVec2(corners[0]),
        serializeVec2(corners[1]),
        serializeVec2(corners[2]),
        serializeVec2(corners[3])
    });

    j["meshResolution"] = geometry.getMeshResolution();
    j["meshMode"] = static_cast<int>(geometry.getMeshMode());

    // Non-uniform control grid (ADR-038): persist the division coordinates.
    // Uniform grids stay byte-clean (reconstructed from meshResolution on load).
    if (!geometry.isUniform()) {
        j["uCoords"] = geometry.getUCoords();
        j["vCoords"] = geometry.getVCoords();
    }

    // Serialize mesh vertex offsets if any exist
    if (geometry.hasMeshModifications()) {
        const auto& meshVertices = geometry.getMeshVertices();
        nlohmann::json meshOffsetsJson = nlohmann::json::array();
        for (const auto& offset : meshVertices) {
            meshOffsetsJson.push_back(serializeVec2(offset));
        }
        j["meshOffsets"] = meshOffsetsJson;
    }

    return j;
}

// Helper to deserialize quad geometry
static void deserializeQuadGeometry(const nlohmann::json& j, QuadGeometry& geometry) {
    if (j.contains("corners") && j["corners"].is_array() && j["corners"].size() == 4) {
        std::array<Vec2, 4> corners;
        for (int i = 0; i < 4; ++i) {
            corners[i] = deserializeVec2(j["corners"][i]);
        }
        geometry.setCorners(corners);
    }

    // Control grid (ADR-038): prefer explicit division coords; otherwise migrate
    // the legacy uniform meshResolution into an even grid. Either way this sets
    // the vertex count before the offsets below are applied.
    if (j.contains("uCoords") && j["uCoords"].is_array() &&
        j.contains("vCoords") && j["vCoords"].is_array()) {
        geometry.setGridCoords(j["uCoords"].get<std::vector<float>>(),
                               j["vCoords"].get<std::vector<float>>());
    } else if (j.contains("meshResolution")) {
        geometry.setMeshResolution(j["meshResolution"].get<int>());
    }

    if (j.contains("meshMode")) {
        geometry.setMeshMode(static_cast<MeshMode>(j["meshMode"].get<int>()));
    }

    // Deserialize mesh vertex offsets if present
    if (j.contains("meshOffsets") && j["meshOffsets"].is_array()) {
        const auto& offsetsJson = j["meshOffsets"];
        int expectedCount = geometry.getVertexCount();
        if (static_cast<int>(offsetsJson.size()) == expectedCount) {
            std::vector<Vec2> offsets;
            offsets.reserve(expectedCount);
            for (const auto& offsetJson : offsetsJson) {
                offsets.push_back(deserializeVec2(offsetJson));
            }
            geometry.setMeshVertices(offsets);
        }
    }
}

// Helper to serialize texture binding UV transforms
static nlohmann::json serializeTextureBinding(const TextureBinding& binding) {
    nlohmann::json j;
    j["uvOffset"] = serializeVec2(binding.uvOffset);
    j["uvScale"] = serializeVec2(binding.uvScale);
    j["uvRotation"] = binding.uvRotation;
    j["flipH"] = binding.flipH;
    j["flipV"] = binding.flipV;
    // ADR-040 crop: written only when non-identity so untouched quads (and all
    // pre-crop files) stay byte-identical.
    if (!binding.isCropIdentity()) {
        j["crop"] = {
            {"x", binding.cropRect.x}, {"y", binding.cropRect.y},
            {"width", binding.cropRect.width}, {"height", binding.cropRect.height},
        };
    }
    return j;
}

// Helper to deserialize texture binding UV transforms
static void deserializeTextureBinding(const nlohmann::json& j, TextureBinding& binding) {
    if (j.contains("uvOffset") && j["uvOffset"].is_array()) {
        binding.uvOffset = deserializeVec2(j["uvOffset"]);
    }
    if (j.contains("uvScale") && j["uvScale"].is_array()) {
        binding.uvScale = deserializeVec2(j["uvScale"]);
    }
    if (j.contains("uvRotation")) {
        binding.uvRotation = j["uvRotation"].get<float>();
    }
    if (j.contains("flipH")) {
        binding.flipH = j["flipH"].get<bool>();
    }
    if (j.contains("flipV")) {
        binding.flipV = j["flipV"].get<bool>();
    }
    if (j.contains("crop") && j["crop"].is_object()) {
        const auto& c = j["crop"];
        if (c.contains("x"))      binding.cropRect.x      = c["x"].get<float>();
        if (c.contains("y"))      binding.cropRect.y      = c["y"].get<float>();
        if (c.contains("width"))  binding.cropRect.width  = c["width"].get<float>();
        if (c.contains("height")) binding.cropRect.height = c["height"].get<float>();
    }
}

// Helper to serialize per-quad color correction (ADR-036). Optional on disk;
// only written when it differs from identity so untouched scenes are unchanged.
static nlohmann::json serializeColorCorrection(const ColorCorrection& cc) {
    nlohmann::json j;
    j["brightness"] = cc.brightness;
    j["contrast"] = cc.contrast;
    j["gamma"] = cc.gamma;
    j["rgbGain"] = serializeVec3(cc.rgbGain);
    return j;
}

// Helper to deserialize per-quad color correction. Each field is optional so
// older files (and partial objects) fall back to identity defaults.
static void deserializeColorCorrection(const nlohmann::json& j, ColorCorrection& cc) {
    if (j.contains("brightness")) cc.brightness = j["brightness"].get<float>();
    if (j.contains("contrast"))   cc.contrast   = j["contrast"].get<float>();
    if (j.contains("gamma"))      cc.gamma      = j["gamma"].get<float>();
    if (j.contains("rgbGain") && j["rgbGain"].is_array()) {
        cc.rgbGain = deserializeVec3(j["rgbGain"]);
    }
}

// Helper to serialize per-quad soft-edge blending (ADR-037). Optional on disk;
// only written when non-identity so untouched scenes are unchanged.
static nlohmann::json serializeSoftEdge(const SoftEdge& se) {
    nlohmann::json j;
    j["left"] = se.left;
    j["right"] = se.right;
    j["top"] = se.top;
    j["bottom"] = se.bottom;
    j["gamma"] = se.gamma;
    return j;
}

// Helper to deserialize per-quad soft-edge blending. Each field is optional so
// older files (and partial objects) fall back to identity (no feather) defaults.
static void deserializeSoftEdge(const nlohmann::json& j, SoftEdge& se) {
    if (j.contains("left"))   se.left   = j["left"].get<float>();
    if (j.contains("right"))  se.right  = j["right"].get<float>();
    if (j.contains("top"))    se.top    = j["top"].get<float>();
    if (j.contains("bottom")) se.bottom = j["bottom"].get<float>();
    if (j.contains("gamma"))  se.gamma  = j["gamma"].get<float>();
}

// Helper to serialize a single quad
static nlohmann::json serializeQuad(const Quad& quad, const std::string& texturePath) {
    nlohmann::json j;

    j["id"] = quad.getId().toString();
    j["name"] = quad.getName();
    j["zOrder"] = quad.getZOrder();
    j["visible"] = quad.isVisible();
    j["locked"] = quad.isLocked();
    j["opacity"] = quad.getOpacity();
    j["geometry"] = serializeQuadGeometry(quad.getGeometry());
    j["textureBinding"] = serializeTextureBinding(quad.getTexture());
    if (!quad.getColorCorrection().isIdentity()) {
        j["colorCorrection"] = serializeColorCorrection(quad.getColorCorrection());
    }
    if (!quad.getSoftEdge().isIdentity()) {
        j["softEdge"] = serializeSoftEdge(quad.getSoftEdge());
    }

    if (!texturePath.empty()) {
        j["texturePath"] = texturePath;
    }

    return j;
}

// Parse an "id" field defensively: a missing or malformed id (older/hand-edited files)
// yields null so the caller falls back to a fresh id instead of throwing.
static UUID parseIdOrNull(const nlohmann::json& j) {
    if (!j.contains("id") || !j["id"].is_string()) return UUID();
    const std::string s = j["id"].get<std::string>();
    if (s.length() != 36) return UUID();
    try { return UUID::fromString(s); } catch (...) { return UUID(); }
}

// Helper to deserialize a single quad
static Quad deserializeQuad(const nlohmann::json& j, std::string& texturePath) {
    // Preserve the persisted id so group membership (which references quad ids by value)
    // re-links to the right quad on load. The id has always been written; older files
    // without it fall back to a fresh id.
    UUID id = parseIdOrNull(j);
    Quad quad = id.isNull() ? Quad() : Quad(id);

    if (j.contains("name")) {
        quad.setName(j["name"].get<std::string>());
    }
    if (j.contains("zOrder")) {
        quad.setZOrder(j["zOrder"].get<int>());
    }
    if (j.contains("visible")) {
        quad.setVisible(j["visible"].get<bool>());
    }
    if (j.contains("locked")) {
        quad.setLocked(j["locked"].get<bool>());
    }
    if (j.contains("opacity")) {
        quad.setOpacity(j["opacity"].get<float>());
    }
    if (j.contains("geometry")) {
        deserializeQuadGeometry(j["geometry"], quad.getGeometry());
    }
    if (j.contains("textureBinding")) {
        deserializeTextureBinding(j["textureBinding"], quad.getTexture());
    }
    if (j.contains("colorCorrection")) {
        ColorCorrection cc;
        deserializeColorCorrection(j["colorCorrection"], cc);
        quad.setColorCorrection(cc);  // clamps to valid ranges
    }
    if (j.contains("softEdge")) {
        SoftEdge se;
        deserializeSoftEdge(j["softEdge"], se);
        quad.setSoftEdge(se);  // clamps to valid ranges
    }
    if (j.contains("texturePath")) {
        texturePath = j["texturePath"].get<std::string>();
    } else {
        texturePath.clear();
    }

    return quad;
}

// Helper to serialize a single group
static nlohmann::json serializeGroup(const QuadGroup& group) {
    nlohmann::json j;

    j["id"] = group.getId().toString();
    j["name"] = group.getName();
    j["visible"] = group.isVisible();
    j["locked"] = group.isLocked();
    j["collapsed"] = group.isCollapsed();

    // Serialize member quad IDs
    nlohmann::json membersJson = nlohmann::json::array();
    for (const UUID& memberId : group.getMemberQuadIds()) {
        membersJson.push_back(memberId.toString());
    }
    j["members"] = membersJson;

    // Serialize parent group if present
    if (group.getParentGroup().has_value()) {
        j["parentGroup"] = group.getParentGroup()->toString();
    }

    return j;
}

// Helper to deserialize a single group
static QuadGroup deserializeGroup(const nlohmann::json& j) {
    // Preserve the persisted id so nested-group parent references re-link on load.
    UUID id = parseIdOrNull(j);
    QuadGroup group = id.isNull() ? QuadGroup() : QuadGroup(id);

    if (j.contains("name")) {
        group.setName(j["name"].get<std::string>());
    }
    if (j.contains("visible")) {
        group.setVisible(j["visible"].get<bool>());
    }
    if (j.contains("locked")) {
        group.setLocked(j["locked"].get<bool>());
    }
    if (j.contains("collapsed")) {
        group.setCollapsed(j["collapsed"].get<bool>());
    }

    // Deserialize member quad IDs
    if (j.contains("members") && j["members"].is_array()) {
        for (const auto& memberJson : j["members"]) {
            UUID memberId = UUID::fromString(memberJson.get<std::string>());
            group.addMember(memberId);
        }
    }

    // Deserialize parent group if present
    if (j.contains("parentGroup")) {
        UUID parentId = UUID::fromString(j["parentGroup"].get<std::string>());
        group.setParentGroup(parentId);
    }

    return group;
}

// Append one quad (and its texture path) to a scene DTO.
static void loadQuadIntoScene(Project::SceneData& scene, const nlohmann::json& quadJson) {
    std::string texturePath;
    Quad quad = deserializeQuad(quadJson, texturePath);
    scene.quads.push_back(std::move(quad));
    scene.texturePaths.push_back(texturePath);
}

// Serialize one scene DTO to a v3 "scenes" element.
static nlohmann::json serializeScene(const Project::SceneData& scene) {
    nlohmann::json j;
    j["id"] = scene.id.toString();
    j["name"] = scene.name;
    if (!scene.assignedOutputId.isNull()) {
        j["assignedOutputId"] = scene.assignedOutputId.toString();
    }
    j["view"] = {
        {"pan", serializeVec2(scene.view.pan)},
        {"zoom", scene.view.zoom},
        {"gridVisible", scene.view.gridVisible},
        {"snapToGrid", scene.view.snapToGrid},
    };

    nlohmann::json quadsJson = nlohmann::json::array();
    for (size_t i = 0; i < scene.quads.size(); ++i) {
        std::string texPath = (i < scene.texturePaths.size()) ? scene.texturePaths[i] : "";
        quadsJson.push_back(serializeQuad(scene.quads[i], texPath));
    }
    j["quads"] = quadsJson;

    if (!scene.groups.empty()) {
        nlohmann::json groupsJson = nlohmann::json::array();
        for (const auto& group : scene.groups) {
            groupsJson.push_back(serializeGroup(group));
        }
        j["groups"] = groupsJson;
    }
    return j;
}

// Deserialize a v2/v3 "scenes" element into a scene DTO. v2 elements simply lack
// the v3-only fields (assignedOutputId/view/id), which fall back to defaults.
static Project::SceneData deserializeScene(const nlohmann::json& j) {
    Project::SceneData scene;
    if (j.contains("id")) {
        scene.id = UUID::fromString(j["id"].get<std::string>());
    }
    if (scene.id.isNull()) {
        scene.id = UUID::generate();  // generate a fresh id so every scene is addressable
    }
    scene.name = j.contains("name") ? j["name"].get<std::string>() : "Scene";
    if (j.contains("assignedOutputId")) {
        scene.assignedOutputId = UUID::fromString(j["assignedOutputId"].get<std::string>());
    }
    if (j.contains("view") && j["view"].is_object()) {
        const auto& v = j["view"];
        if (v.contains("pan") && v["pan"].is_array()) scene.view.pan = deserializeVec2(v["pan"]);
        if (v.contains("zoom")) scene.view.zoom = v["zoom"].get<float>();
        if (v.contains("gridVisible")) scene.view.gridVisible = v["gridVisible"].get<bool>();
        if (v.contains("snapToGrid")) scene.view.snapToGrid = v["snapToGrid"].get<bool>();
    }
    if (j.contains("quads") && j["quads"].is_array()) {
        for (const auto& quadJson : j["quads"]) {
            loadQuadIntoScene(scene, quadJson);
        }
    }
    if (j.contains("groups") && j["groups"].is_array()) {
        for (const auto& groupJson : j["groups"]) {
            scene.groups.push_back(deserializeGroup(groupJson));
        }
    }
    return scene;
}

static nlohmann::json serializeOutput(const Project::OutputData& output) {
    nlohmann::json j;
    j["id"] = output.id.toString();
    j["name"] = output.name;
    j["monitor"] = output.monitor;
    j["resolution"] = nlohmann::json::array({output.resolution.x, output.resolution.y});
    j["rotation"] = output.rotationDegrees;
    j["enabled"] = output.enabled;
    return j;
}

static Project::OutputData deserializeOutput(const nlohmann::json& j) {
    Project::OutputData output;
    if (j.contains("id")) {
        output.id = UUID::fromString(j["id"].get<std::string>());
    }
    if (output.id.isNull()) {
        output.id = UUID::generate();
    }
    output.name = j.contains("name") ? j["name"].get<std::string>() : "Output";
    if (j.contains("monitor")) output.monitor = j["monitor"].get<int>();
    if (j.contains("resolution") && j["resolution"].is_array() && j["resolution"].size() == 2) {
        output.resolution = IVec2(j["resolution"][0].get<int>(), j["resolution"][1].get<int>());
    }
    if (j.contains("rotation")) output.rotationDegrees = j["rotation"].get<int>();
    if (j.contains("enabled")) output.enabled = j["enabled"].get<bool>();
    return output;
}

// Rebuild the flat quads_/texturePaths_/groups_ as the union of every scene, so
// flat consumers (and the existing flat-API tests) keep working after a v3 load.
static void flattenScenesToFlat(Project& project) {
    project.clearQuads();
    project.getTexturePaths().clear();
    project.clearGroups();
    for (const auto& scene : project.getScenes()) {
        for (size_t i = 0; i < scene.quads.size(); ++i) {
            project.getQuads().push_back(scene.quads[i]);
            project.getTexturePaths().push_back(
                i < scene.texturePaths.size() ? scene.texturePaths[i] : std::string());
        }
        for (const auto& group : scene.groups) {
            project.getGroups().push_back(group);
        }
    }
}

// Load a JSON document into the Project model (ADR-032 / ADR-039). Every read path
// yields >= 1 scene: v1.x flat "quads" wrap into one scene; v2.x/v3.x "scenes" map
// directly; v3.x also restores the "outputs" array. The flat quads_/groups_ are then
// derived from the scenes so flat consumers stay backward-compatible.
static void loadProjectModel(const nlohmann::json& json, Project& project) {
    if (json.contains("name")) {
        project.setName(json["name"].get<std::string>());
    }

    auto& scenes = project.getScenes();
    if (json.contains("scenes") && json["scenes"].is_array()) {
        for (const auto& sceneJson : json["scenes"]) {
            scenes.push_back(deserializeScene(sceneJson));
        }
    } else {
        // v1.x flat: wrap the top-level quads/groups into a single scene.
        Project::SceneData scene;
        scene.id = UUID::generate();
        scene.name = "Scene 1";
        if (json.contains("quads") && json["quads"].is_array()) {
            for (const auto& quadJson : json["quads"]) {
                loadQuadIntoScene(scene, quadJson);
            }
        }
        if (json.contains("groups") && json["groups"].is_array()) {
            for (const auto& groupJson : json["groups"]) {
                scene.groups.push_back(deserializeGroup(groupJson));
            }
        }
        scenes.push_back(std::move(scene));
    }

    // Guarantee at least one scene so the editor is never sceneless.
    if (scenes.empty()) {
        Project::SceneData scene;
        scene.id = UUID::generate();
        scene.name = "Scene 1";
        scenes.push_back(std::move(scene));
    }

    if (json.contains("outputs") && json["outputs"].is_array()) {
        for (const auto& outputJson : json["outputs"]) {
            project.getOutputs().push_back(deserializeOutput(outputJson));
        }
    }

    flattenScenesToFlat(project);
}

// Build the on-disk JSON for a project. Writes v3 (scenes + outputs) when the
// project carries scene DTOs; otherwise the legacy v1.2 flat format, so a flat-only
// Project (and the existing flat round-trip tests) is byte-compatible with before.
static nlohmann::json buildProjectJson(const Project& project) {
    nlohmann::json json;
    json["$schema"] = "quadmapper/project/1.0";

    if (!project.getScenes().empty()) {
        json["version"] = "3.0.0";  // 3.0: multi-scene + per-output routing (ADR-039)
        json["name"] = project.getName();

        nlohmann::json scenesJson = nlohmann::json::array();
        for (const auto& scene : project.getScenes()) {
            scenesJson.push_back(serializeScene(scene));
        }
        json["scenes"] = scenesJson;

        if (!project.getOutputs().empty()) {
            nlohmann::json outputsJson = nlohmann::json::array();
            for (const auto& output : project.getOutputs()) {
                outputsJson.push_back(serializeOutput(output));
            }
            json["outputs"] = outputsJson;
        }
        return json;
    }

    json["version"] = "1.2.0";  // 1.2: additive non-uniform control grid (ADR-038)
    json["name"] = project.getName();

    const auto& quads = project.getQuads();
    const auto& texturePaths = project.getTexturePaths();
    nlohmann::json quadsJson = nlohmann::json::array();
    for (size_t i = 0; i < quads.size(); ++i) {
        std::string texPath = (i < texturePaths.size()) ? texturePaths[i] : "";
        quadsJson.push_back(serializeQuad(quads[i], texPath));
    }
    json["quads"] = quadsJson;

    const auto& groups = project.getGroups();
    if (!groups.empty()) {
        nlohmann::json groupsJson = nlohmann::json::array();
        for (const auto& group : groups) {
            groupsJson.push_back(serializeGroup(group));
        }
        json["groups"] = groupsJson;
    }
    return json;
}

Result<void, Error> ProjectSerializer::save(const Project& project, const std::filesystem::path& path) {
    nlohmann::json json = buildProjectJson(project);
    Logger::info("Saving project to: {}", path.string());
    return FileSystem::writeFile(path, json.dump(2));
}

Result<Project, Error> ProjectSerializer::load(const std::filesystem::path& path) {
    Logger::info("Loading project from: {}", path.string());

    auto result = FileSystem::readFile(path);
    if (!result) {
        return Err<Project>(result.error().message);
    }

    Project project;
    try {
        auto json = nlohmann::json::parse(result.value());

        auto versionResult = validateVersion(json);
        if (versionResult.isError()) {
            return Err<Project>(versionResult.error());
        }

        loadProjectModel(json, project);

        project.setFilePath(path.string());
        project.setModified(false);

        Logger::info("Loaded project '{}' with {} scene(s), {} quads and {} groups",
                     project.getName(), project.getScenes().size(),
                     project.getQuads().size(), project.getGroups().size());

    } catch (const std::exception& e) {
        return Err<Project>(std::string("Failed to parse project: ") + e.what());
    }

    return Ok(std::move(project));
}

std::string ProjectSerializer::serialize(const Project& project) {
    return buildProjectJson(project).dump(2);
}

Result<void, std::string> ProjectSerializer::deserialize(const std::string& jsonStr, Project& project) {
    if (jsonStr.empty()) {
        return Result<void, std::string>::error("Empty JSON string");
    }

    try {
        auto json = nlohmann::json::parse(jsonStr);

        auto versionResult = validateVersion(json);
        if (versionResult.isError()) {
            return Result<void, std::string>::error(versionResult.error());
        }

        // Clear existing data, then load.
        project.clearQuads();
        project.getTexturePaths().clear();
        project.clearGroups();
        project.getScenes().clear();
        project.getOutputs().clear();

        loadProjectModel(json, project);

        project.setModified(false);

        return Result<void, std::string>::success();
    } catch (const std::exception& e) {
        return Result<void, std::string>::error(std::string("Parse error: ") + e.what());
    }
}

} // namespace quadmapper
