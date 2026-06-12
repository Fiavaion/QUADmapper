#pragma once

#include "core/Result.hpp"
#include "core/Types.hpp"
#include "Project.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneManager.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadGeometry.hpp"
#include "scene/QuadGroup.hpp"
#include <filesystem>
#include <string>

namespace quadmapper {

class ProjectSerializer {
public:
    static Result<void, Error> save(const Project& project, const std::filesystem::path& path);
    static Result<Project, Error> load(const std::filesystem::path& path);

    // String-based serialization for testing
    std::string serialize(const Project& project);
    Result<void, std::string> deserialize(const std::string& json, Project& project);

    // Scene-based serialization (for use with SceneManager)
    static Result<void, Error> saveWithScenes(const std::string& projectName,
                                               const SceneManager& sceneManager,
                                               const std::filesystem::path& path);
    static Result<void, Error> loadToSceneManager(const std::filesystem::path& path,
                                                   std::string& projectName,
                                                   SceneManager& sceneManager);

    // JSON-based scene serialization for testing
    std::string serializeScenes(const std::string& projectName, const SceneManager& sceneManager);
    Result<void, std::string> deserializeScenes(const std::string& json,
                                                 std::string& projectName,
                                                 SceneManager& sceneManager);
};

} // namespace quadmapper
