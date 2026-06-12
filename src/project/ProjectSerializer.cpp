#include "ProjectSerializer.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneManager.hpp"
#include <nlohmann/json.hpp>

namespace quadmapper {

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

// Helper to deserialize a single scene into a SceneManager
static void deserializeScene(const nlohmann::json& j, SceneManager& sceneManager) {
    std::string name = "New Scene";
    if (j.contains("name")) {
        name = j["name"].get<std::string>();
    }

    Scene* scene = sceneManager.createScene(name);

    // Deserialize view state
    if (j.contains("viewState")) {
        scene->getViewState() = deserializeSceneViewState(j["viewState"]);
    }

    // Deserialize quads
    if (j.contains("quads") && j["quads"].is_array()) {
        for (const auto& quadJson : j["quads"]) {
            std::string texturePath;
            Quad deserializedQuad = deserializeQuad(quadJson, texturePath);

            // Create quad in scene and copy properties
            Quad* newQuad = scene->createQuad();
            newQuad->setName(deserializedQuad.getName());
            newQuad->setZOrder(deserializedQuad.getZOrder());
            newQuad->setVisible(deserializedQuad.isVisible());
            newQuad->setLocked(deserializedQuad.isLocked());
            newQuad->setOpacity(deserializedQuad.getOpacity());

            // Copy geometry
            auto& newGeometry = newQuad->getGeometry();
            const auto& srcGeometry = deserializedQuad.getGeometry();
            newGeometry.setCorners(srcGeometry.getCorners());
            newGeometry.setMeshResolution(srcGeometry.getMeshResolution());
            newGeometry.setMeshMode(srcGeometry.getMeshMode());

            // Copy texture binding
            newQuad->getTexture() = deserializedQuad.getTexture();
        }
    }

    // Deserialize groups
    if (j.contains("groups") && j["groups"].is_array()) {
        for (const auto& groupJson : j["groups"]) {
            QuadGroup deserializedGroup = deserializeGroup(groupJson);

            // Create group in scene
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

    // Handle isActive flag
    if (j.contains("isActive") && j["isActive"].get<bool>()) {
        sceneManager.setActiveScene(scene->getId());
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

        // Check version
        if (!json.contains("version")) {
            return Err<Project>("Missing version field in project file");
        }
        std::string version = json["version"];
        if (version.substr(0, 2) != "1.") {
            return Err<Project>("Incompatible project version: " + version);
        }

        // Load project name
        if (json.contains("name")) {
            project.setName(json["name"]);
        }

        // Load quads
        if (json.contains("quads") && json["quads"].is_array()) {
            for (const auto& quadJson : json["quads"]) {
                std::string texturePath;
                Quad quad = deserializeQuad(quadJson, texturePath);
                project.getQuads().push_back(std::move(quad));
                project.getTexturePaths().push_back(texturePath);
            }
        }

        // Load groups
        if (json.contains("groups") && json["groups"].is_array()) {
            for (const auto& groupJson : json["groups"]) {
                QuadGroup group = deserializeGroup(groupJson);
                project.getGroups().push_back(std::move(group));
            }
        }

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

        if (!json.contains("version")) {
            return Result<void, std::string>::error("Missing version field");
        }

        std::string version = json["version"];
        if (version.substr(0, 2) != "1.") {
            return Result<void, std::string>::error("Incompatible version: " + version);
        }

        if (json.contains("name")) {
            project.setName(json["name"]);
        }

        // Clear existing data and load new ones
        project.clearQuads();
        project.getTexturePaths().clear();
        project.clearGroups();

        if (json.contains("quads") && json["quads"].is_array()) {
            for (const auto& quadJson : json["quads"]) {
                std::string texturePath;
                Quad quad = deserializeQuad(quadJson, texturePath);
                project.getQuads().push_back(std::move(quad));
                project.getTexturePaths().push_back(texturePath);
            }
        }

        // Load groups
        if (json.contains("groups") && json["groups"].is_array()) {
            for (const auto& groupJson : json["groups"]) {
                QuadGroup group = deserializeGroup(groupJson);
                project.getGroups().push_back(std::move(group));
            }
        }

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
    json["$schema"] = "quadmapper/project/2.0";
    json["version"] = "2.0.0";
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

        // Check version
        if (!json.contains("version")) {
            return Err<void>("Missing version field in project file");
        }
        std::string version = json["version"];

        // Load project name
        if (json.contains("name")) {
            projectName = json["name"].get<std::string>();
        } else {
            projectName = "Untitled Project";
        }

        // Reset scene manager to clean state
        sceneManager.reset();

        // Check for v2.x format with scenes array
        if (json.contains("scenes") && json["scenes"].is_array()) {
            // Version 2.x format - load scenes directly
            // First, remove the default scene created by reset()
            if (sceneManager.getSceneCount() > 0) {
                // We need to handle this differently - reset creates a default scene
                // Delete all scenes except we can't delete the last one
                // So we'll load the first scene into it and create additional scenes
            }

            const auto& scenesArray = json["scenes"];
            bool firstScene = true;
            for (const auto& sceneJson : scenesArray) {
                if (firstScene) {
                    // Update the existing default scene
                    Scene* scene = sceneManager.getSceneByIndex(0);
                    if (scene) {
                        if (sceneJson.contains("name")) {
                            scene->setName(sceneJson["name"].get<std::string>());
                        }
                        if (sceneJson.contains("viewState")) {
                            scene->getViewState() = deserializeSceneViewState(sceneJson["viewState"]);
                        }
                        // Load quads into existing scene
                        if (sceneJson.contains("quads") && sceneJson["quads"].is_array()) {
                            for (const auto& quadJson : sceneJson["quads"]) {
                                std::string texturePath;
                                Quad deserializedQuad = deserializeQuad(quadJson, texturePath);
                                Quad* newQuad = scene->createQuad();
                                newQuad->setName(deserializedQuad.getName());
                                newQuad->setZOrder(deserializedQuad.getZOrder());
                                newQuad->setVisible(deserializedQuad.isVisible());
                                newQuad->setLocked(deserializedQuad.isLocked());
                                newQuad->setOpacity(deserializedQuad.getOpacity());
                                auto& newGeometry = newQuad->getGeometry();
                                const auto& srcGeometry = deserializedQuad.getGeometry();
                                newGeometry.setCorners(srcGeometry.getCorners());
                                newGeometry.setMeshResolution(srcGeometry.getMeshResolution());
                                newGeometry.setMeshMode(srcGeometry.getMeshMode());
                                newQuad->getTexture() = deserializedQuad.getTexture();
                            }
                        }
                        // Handle isActive
                        if (sceneJson.contains("isActive") && sceneJson["isActive"].get<bool>()) {
                            sceneManager.setActiveScene(scene->getId());
                        }
                    }
                    firstScene = false;
                } else {
                    // Create additional scenes
                    deserializeScene(sceneJson, sceneManager);
                }
            }

            Logger::info("Loaded project '{}' with {} scenes (v2.x format)",
                         projectName, sceneManager.getSceneCount());
        }
        else if (json.contains("quads")) {
            // Version 1.x format - migrate quads to single scene
            // The default scene already exists from reset()
            Scene* scene = sceneManager.getSceneByIndex(0);
            if (scene) {
                scene->setName("Scene 1");

                if (json["quads"].is_array()) {
                    for (const auto& quadJson : json["quads"]) {
                        std::string texturePath;
                        Quad deserializedQuad = deserializeQuad(quadJson, texturePath);
                        Quad* newQuad = scene->createQuad();
                        newQuad->setName(deserializedQuad.getName());
                        newQuad->setZOrder(deserializedQuad.getZOrder());
                        newQuad->setVisible(deserializedQuad.isVisible());
                        newQuad->setLocked(deserializedQuad.isLocked());
                        newQuad->setOpacity(deserializedQuad.getOpacity());
                        auto& newGeometry = newQuad->getGeometry();
                        const auto& srcGeometry = deserializedQuad.getGeometry();
                        newGeometry.setCorners(srcGeometry.getCorners());
                        newGeometry.setMeshResolution(srcGeometry.getMeshResolution());
                        newGeometry.setMeshMode(srcGeometry.getMeshMode());
                        newQuad->getTexture() = deserializedQuad.getTexture();
                    }
                }
            }

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
    json["$schema"] = "quadmapper/project/2.0";
    json["version"] = "2.0.0";
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

        if (!json.contains("version")) {
            return Result<void, std::string>::error("Missing version field");
        }

        if (json.contains("name")) {
            projectName = json["name"].get<std::string>();
        } else {
            projectName = "Untitled Project";
        }

        // Reset scene manager
        sceneManager.reset();

        if (json.contains("scenes") && json["scenes"].is_array()) {
            const auto& scenesArray = json["scenes"];
            bool firstScene = true;
            for (const auto& sceneJson : scenesArray) {
                if (firstScene) {
                    Scene* scene = sceneManager.getSceneByIndex(0);
                    if (scene) {
                        if (sceneJson.contains("name")) {
                            scene->setName(sceneJson["name"].get<std::string>());
                        }
                        if (sceneJson.contains("viewState")) {
                            scene->getViewState() = deserializeSceneViewState(sceneJson["viewState"]);
                        }
                        if (sceneJson.contains("quads") && sceneJson["quads"].is_array()) {
                            for (const auto& quadJson : sceneJson["quads"]) {
                                std::string texturePath;
                                Quad deserializedQuad = deserializeQuad(quadJson, texturePath);
                                Quad* newQuad = scene->createQuad();
                                newQuad->setName(deserializedQuad.getName());
                                newQuad->setZOrder(deserializedQuad.getZOrder());
                                newQuad->setVisible(deserializedQuad.isVisible());
                                newQuad->setLocked(deserializedQuad.isLocked());
                                newQuad->setOpacity(deserializedQuad.getOpacity());
                                auto& newGeometry = newQuad->getGeometry();
                                const auto& srcGeometry = deserializedQuad.getGeometry();
                                newGeometry.setCorners(srcGeometry.getCorners());
                                newGeometry.setMeshResolution(srcGeometry.getMeshResolution());
                                newGeometry.setMeshMode(srcGeometry.getMeshMode());
                                newQuad->getTexture() = deserializedQuad.getTexture();
                            }
                        }
                        if (sceneJson.contains("isActive") && sceneJson["isActive"].get<bool>()) {
                            sceneManager.setActiveScene(scene->getId());
                        }
                    }
                    firstScene = false;
                } else {
                    deserializeScene(sceneJson, sceneManager);
                }
            }
        } else if (json.contains("quads")) {
            // V1.x backward compatibility
            Scene* scene = sceneManager.getSceneByIndex(0);
            if (scene && json["quads"].is_array()) {
                for (const auto& quadJson : json["quads"]) {
                    std::string texturePath;
                    Quad deserializedQuad = deserializeQuad(quadJson, texturePath);
                    Quad* newQuad = scene->createQuad();
                    newQuad->setName(deserializedQuad.getName());
                    newQuad->setZOrder(deserializedQuad.getZOrder());
                    newQuad->setVisible(deserializedQuad.isVisible());
                    newQuad->setLocked(deserializedQuad.isLocked());
                    newQuad->setOpacity(deserializedQuad.getOpacity());
                    auto& newGeometry = newQuad->getGeometry();
                    const auto& srcGeometry = deserializedQuad.getGeometry();
                    newGeometry.setCorners(srcGeometry.getCorners());
                    newGeometry.setMeshResolution(srcGeometry.getMeshResolution());
                    newGeometry.setMeshMode(srcGeometry.getMeshMode());
                    newQuad->getTexture() = deserializedQuad.getTexture();
                }
            }
        }

        return Result<void, std::string>::success();
    } catch (const std::exception& e) {
        return Result<void, std::string>::error(std::string("Parse error: ") + e.what());
    }
}

} // namespace quadmapper
