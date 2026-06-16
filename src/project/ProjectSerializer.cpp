#include "ProjectSerializer.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneManager.hpp"
#include <nlohmann/json.hpp>

namespace quadmapper {

// .qmap format versioning (see ADR-032).
//   1.x = legacy single-quad-list format (no "scenes" array)
//   2.x = current scene-based format ("scenes" array)
// The writer always emits the current version; readers accept any major <= kMaxSupportedMajor
// and migrate older majors up in memory.
static constexpr const char* kCurrentSchema = "quadmapper/project/2.0";
static constexpr const char* kCurrentVersion = "2.0.0";
static constexpr int kMaxSupportedMajor = 2;

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

    if (j.contains("meshResolution")) {
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

    if (!texturePath.empty()) {
        j["texturePath"] = texturePath;
    }

    return j;
}

// Helper to deserialize a single quad
static Quad deserializeQuad(const nlohmann::json& j, std::string& texturePath) {
    Quad quad;

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
    QuadGroup group;

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

// Helper to serialize scene view state
static nlohmann::json serializeSceneViewState(const SceneViewState& viewState) {
    nlohmann::json j;
    j["panOffset"] = serializeVec2(viewState.panOffset);
    j["zoomLevel"] = viewState.zoomLevel;
    j["gridVisible"] = viewState.gridVisible;
    j["snapToGrid"] = viewState.snapToGrid;
    return j;
}

// Helper to deserialize scene view state
static SceneViewState deserializeSceneViewState(const nlohmann::json& j) {
    SceneViewState viewState;
    if (j.contains("panOffset") && j["panOffset"].is_array()) {
        viewState.panOffset = deserializeVec2(j["panOffset"]);
    }
    if (j.contains("zoomLevel")) {
        viewState.zoomLevel = j["zoomLevel"].get<float>();
    }
    if (j.contains("gridVisible")) {
        viewState.gridVisible = j["gridVisible"].get<bool>();
    }
    if (j.contains("snapToGrid")) {
        viewState.snapToGrid = j["snapToGrid"].get<bool>();
    }
    return viewState;
}

// Helper to serialize a single scene
static nlohmann::json serializeScene(const Scene& scene, const std::vector<std::string>& texturePaths) {
    nlohmann::json j;

    j["id"] = scene.getId().toString();
    j["name"] = scene.getName();
    j["isActive"] = scene.isActive();
    j["viewState"] = serializeSceneViewState(scene.getViewState());

    // Serialize quads
    const auto& quads = scene.getQuads();
    nlohmann::json quadsJson = nlohmann::json::array();
    for (size_t i = 0; i < quads.size(); ++i) {
        std::string texPath = (i < texturePaths.size()) ? texturePaths[i] : "";
        quadsJson.push_back(serializeQuad(*quads[i], texPath));
    }
    j["quads"] = quadsJson;

    // Serialize groups
    const auto& groups = scene.getGroups();
    if (!groups.empty()) {
        nlohmann::json groupsJson = nlohmann::json::array();
        for (const auto& group : groups) {
            groupsJson.push_back(serializeGroup(*group));
        }
        j["groups"] = groupsJson;
    }

    return j;
}

// Load a single quad from JSON into an existing scene. Copies the whole geometry (corners,
// resolution, mode AND mesh vertex offsets) so mesh-edit warps survive a round-trip.
static void loadQuadInto(Scene& scene, const nlohmann::json& quadJson) {
    std::string texturePath;
    Quad src = deserializeQuad(quadJson, texturePath);

    Quad* newQuad = scene.createQuad();
    newQuad->setName(src.getName());
    newQuad->setZOrder(src.getZOrder());
    newQuad->setVisible(src.isVisible());
    newQuad->setLocked(src.isLocked());
    newQuad->setOpacity(src.getOpacity());
    newQuad->getGeometry() = src.getGeometry();
    newQuad->getTexture() = src.getTexture();
}

// Load a scene's contents (name, view state, quads, groups, active flag) into an existing
// Scene. The single de-dup point for every scene-load path (ADR-032).
static void loadSceneContents(const nlohmann::json& j, Scene* scene, SceneManager& sceneManager) {
    if (!scene) {
        return;
    }

    if (j.contains("name")) {
        scene->setName(j["name"].get<std::string>());
    }

    if (j.contains("viewState")) {
        scene->getViewState() = deserializeSceneViewState(j["viewState"]);
    }

    if (j.contains("quads") && j["quads"].is_array()) {
        for (const auto& quadJson : j["quads"]) {
            loadQuadInto(*scene, quadJson);
        }
    }

    if (j.contains("groups") && j["groups"].is_array()) {
        for (const auto& groupJson : j["groups"]) {
            QuadGroup deserializedGroup = deserializeGroup(groupJson);

            QuadGroup* newGroup = scene->createGroup(deserializedGroup.getName());
            newGroup->setVisible(deserializedGroup.isVisible());
            newGroup->setLocked(deserializedGroup.isLocked());
            newGroup->setCollapsed(deserializedGroup.isCollapsed());

            // Copy member IDs (note: these reference original quad IDs which may not match)
            for (const UUID& memberId : deserializedGroup.getMemberQuadIds()) {
                newGroup->addMember(memberId);
            }
        }
    }

    if (j.contains("isActive") && j["isActive"].get<bool>()) {
        sceneManager.setActiveScene(scene->getId());
    }
}

// Load a scene from JSON, creating a new scene in the manager.
static void deserializeScene(const nlohmann::json& j, SceneManager& sceneManager) {
    std::string name = "New Scene";
    if (j.contains("name")) {
        name = j["name"].get<std::string>();
    }
    Scene* scene = sceneManager.createScene(name);
    loadSceneContents(j, scene, sceneManager);
}

// Load all scenes from a v2.x "scenes" array into the manager, reusing the default scene
// (index 0) created by SceneManager::reset() for the first entry. Shared by file and string
// load paths (ADR-032).
static void loadScenesArray(const nlohmann::json& scenesArray, SceneManager& sceneManager) {
    bool firstScene = true;
    for (const auto& sceneJson : scenesArray) {
        if (firstScene) {
            loadSceneContents(sceneJson, sceneManager.getSceneByIndex(0), sceneManager);
            firstScene = false;
        } else {
            deserializeScene(sceneJson, sceneManager);
        }
    }
}

// Migrate a v1.x flat "quads" array into the manager's default scene (index 0). ADR-032.
static void loadV1QuadsAsScene(const nlohmann::json& json, SceneManager& sceneManager) {
    Scene* scene = sceneManager.getSceneByIndex(0);
    if (!scene) {
        return;
    }
    scene->setName("Scene 1");
    if (json.contains("quads") && json["quads"].is_array()) {
        for (const auto& quadJson : json["quads"]) {
            loadQuadInto(*scene, quadJson);
        }
    }
}

// Append one quad (and its texture path) to the flat Project model.
static void loadQuadIntoProject(Project& project, const nlohmann::json& quadJson) {
    std::string texturePath;
    Quad quad = deserializeQuad(quadJson, texturePath);
    project.getQuads().push_back(std::move(quad));
    project.getTexturePaths().push_back(texturePath);
}

// Load a JSON document into the flat Project model. Handles both v1.x (top-level "quads")
// and v2.x (flattens every scene's quads, since the flat model has no scene concept) so a
// v2.x file never loads as silently empty. Shared by load() and deserialize() (ADR-032).
static void loadFlatProject(const nlohmann::json& json, Project& project) {
    if (json.contains("name")) {
        project.setName(json["name"].get<std::string>());
    }

    if (json.contains("scenes") && json["scenes"].is_array()) {
        for (const auto& sceneJson : json["scenes"]) {
            if (sceneJson.contains("quads") && sceneJson["quads"].is_array()) {
                for (const auto& quadJson : sceneJson["quads"]) {
                    loadQuadIntoProject(project, quadJson);
                }
            }
        }
    } else if (json.contains("quads") && json["quads"].is_array()) {
        for (const auto& quadJson : json["quads"]) {
            loadQuadIntoProject(project, quadJson);
        }
    }

    if (json.contains("groups") && json["groups"].is_array()) {
        for (const auto& groupJson : json["groups"]) {
            project.getGroups().push_back(deserializeGroup(groupJson));
        }
    }
}

Result<void, Error> ProjectSerializer::save(const Project& project, const std::filesystem::path& path) {
    nlohmann::json json;
    json["$schema"] = "quadmapper/project/1.0";
    json["version"] = "1.0.0";
    json["name"] = project.getName();

    // Serialize quads
    const auto& quads = project.getQuads();
    const auto& texturePaths = project.getTexturePaths();

    nlohmann::json quadsJson = nlohmann::json::array();
    for (size_t i = 0; i < quads.size(); ++i) {
        std::string texPath = (i < texturePaths.size()) ? texturePaths[i] : "";
        quadsJson.push_back(serializeQuad(quads[i], texPath));
    }
    json["quads"] = quadsJson;

    // Serialize groups
    const auto& groups = project.getGroups();
    if (!groups.empty()) {
        nlohmann::json groupsJson = nlohmann::json::array();
        for (const auto& group : groups) {
            groupsJson.push_back(serializeGroup(group));
        }
        json["groups"] = groupsJson;
    }

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

        loadFlatProject(json, project);

        project.setFilePath(path.string());
        project.setModified(false);

        Logger::info("Loaded project '{}' with {} quads and {} groups",
                     project.getName(), project.getQuads().size(), project.getGroups().size());

    } catch (const std::exception& e) {
        return Err<Project>(std::string("Failed to parse project: ") + e.what());
    }

    return Ok(std::move(project));
}

std::string ProjectSerializer::serialize(const Project& project) {
    nlohmann::json json;
    json["$schema"] = "quadmapper/project/1.0";
    json["version"] = "1.0.0";
    json["name"] = project.getName();

    const auto& quads = project.getQuads();
    const auto& texturePaths = project.getTexturePaths();

    nlohmann::json quadsJson = nlohmann::json::array();
    for (size_t i = 0; i < quads.size(); ++i) {
        std::string texPath = (i < texturePaths.size()) ? texturePaths[i] : "";
        quadsJson.push_back(serializeQuad(quads[i], texPath));
    }
    json["quads"] = quadsJson;

    // Serialize groups
    const auto& groups = project.getGroups();
    if (!groups.empty()) {
        nlohmann::json groupsJson = nlohmann::json::array();
        for (const auto& group : groups) {
            groupsJson.push_back(serializeGroup(group));
        }
        json["groups"] = groupsJson;
    }

    return json.dump(2);
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

        loadFlatProject(json, project);

        project.setModified(false);

        return Result<void, std::string>::success();
    } catch (const std::exception& e) {
        return Result<void, std::string>::error(std::string("Parse error: ") + e.what());
    }
}

// ============ Scene-based serialization methods ============

Result<void, Error> ProjectSerializer::saveWithScenes(const std::string& projectName,
                                                       const SceneManager& sceneManager,
                                                       const std::filesystem::path& path) {
    nlohmann::json json;
    json["$schema"] = kCurrentSchema;
    json["version"] = kCurrentVersion;
    json["name"] = projectName;

    // Serialize all scenes
    nlohmann::json scenesJson = nlohmann::json::array();
    for (const auto& scenePtr : sceneManager.getScenes()) {
        // For now, pass empty texture paths - texture management is separate
        std::vector<std::string> emptyTexturePaths;
        scenesJson.push_back(serializeScene(*scenePtr, emptyTexturePaths));
    }
    json["scenes"] = scenesJson;

    Logger::info("Saving project '{}' with {} scenes to: {}", projectName, sceneManager.getSceneCount(), path.string());
    return FileSystem::writeFile(path, json.dump(2));
}

Result<void, Error> ProjectSerializer::loadToSceneManager(const std::filesystem::path& path,
                                                           std::string& projectName,
                                                           SceneManager& sceneManager) {
    Logger::info("Loading project from: {}", path.string());

    auto result = FileSystem::readFile(path);
    if (!result) {
        return Err<void>(result.error().message);
    }

    try {
        auto json = nlohmann::json::parse(result.value());

        auto versionResult = validateVersion(json);
        if (versionResult.isError()) {
            return Err<void>(versionResult.error());
        }

        // Load project name
        if (json.contains("name")) {
            projectName = json["name"].get<std::string>();
        } else {
            projectName = "Untitled Project";
        }

        // Reset scene manager to clean state (creates a single default scene at index 0).
        sceneManager.reset();

        if (json.contains("scenes") && json["scenes"].is_array()) {
            // v2.x scene-based format.
            loadScenesArray(json["scenes"], sceneManager);
            Logger::info("Loaded project '{}' with {} scenes (v2.x format)",
                         projectName, sceneManager.getSceneCount());
        } else if (json.contains("quads")) {
            // v1.x flat format - migrate quads into the default scene (ADR-032).
            loadV1QuadsAsScene(json, sceneManager);
            Scene* scene = sceneManager.getSceneByIndex(0);
            Logger::info("Loaded project '{}' with {} quads (migrated from v1.x format)",
                         projectName, scene ? scene->getQuads().size() : 0);
        }

    } catch (const std::exception& e) {
        return Err<void>(std::string("Failed to parse project: ") + e.what());
    }

    return Ok();
}

std::string ProjectSerializer::serializeScenes(const std::string& projectName, const SceneManager& sceneManager) {
    nlohmann::json json;
    json["$schema"] = kCurrentSchema;
    json["version"] = kCurrentVersion;
    json["name"] = projectName;

    nlohmann::json scenesJson = nlohmann::json::array();
    for (const auto& scenePtr : sceneManager.getScenes()) {
        std::vector<std::string> emptyTexturePaths;
        scenesJson.push_back(serializeScene(*scenePtr, emptyTexturePaths));
    }
    json["scenes"] = scenesJson;

    return json.dump(2);
}

Result<void, std::string> ProjectSerializer::deserializeScenes(const std::string& jsonStr,
                                                                std::string& projectName,
                                                                SceneManager& sceneManager) {
    if (jsonStr.empty()) {
        return Result<void, std::string>::error("Empty JSON string");
    }

    try {
        auto json = nlohmann::json::parse(jsonStr);

        auto versionResult = validateVersion(json);
        if (versionResult.isError()) {
            return Result<void, std::string>::error(versionResult.error());
        }

        if (json.contains("name")) {
            projectName = json["name"].get<std::string>();
        } else {
            projectName = "Untitled Project";
        }

        // Reset scene manager (creates a single default scene at index 0).
        sceneManager.reset();

        if (json.contains("scenes") && json["scenes"].is_array()) {
            loadScenesArray(json["scenes"], sceneManager);
        } else if (json.contains("quads")) {
            // v1.x backward compatibility - migrate quads into the default scene (ADR-032).
            loadV1QuadsAsScene(json, sceneManager);
        }

        return Result<void, std::string>::success();
    } catch (const std::exception& e) {
        return Result<void, std::string>::error(std::string("Parse error: ") + e.what());
    }
}

} // namespace quadmapper
