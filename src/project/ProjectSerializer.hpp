#pragma once

#include "core/Result.hpp"
#include "core/Types.hpp"
#include "Project.hpp"
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
};

} // namespace quadmapper
