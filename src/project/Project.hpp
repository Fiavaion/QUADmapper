#pragma once

#include "core/UUID.hpp"
#include "ProjectSettings.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadGroup.hpp"
#include <string>
#include <vector>

namespace quadmapper {

class Project {
public:
    Project();

    const std::string& getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    const std::string& getFilePath() const { return filePath_; }
    void setFilePath(const std::string& path) { filePath_ = path; }

    bool isModified() const { return modified_; }
    void setModified(bool modified) { modified_ = modified; }

    ProjectSettings& getSettings() { return settings_; }
    const ProjectSettings& getSettings() const { return settings_; }

    // Quad management
    std::vector<Quad>& getQuads() { return quads_; }
    const std::vector<Quad>& getQuads() const { return quads_; }

    void addQuad(Quad quad) { quads_.push_back(std::move(quad)); setModified(true); }
    void clearQuads() { quads_.clear(); setModified(true); }

    // Texture path storage (relative paths for portability)
    std::vector<std::string>& getTexturePaths() { return texturePaths_; }
    const std::vector<std::string>& getTexturePaths() const { return texturePaths_; }

    // Group management for serialization
    std::vector<QuadGroup>& getGroups() { return groups_; }
    const std::vector<QuadGroup>& getGroups() const { return groups_; }

    void addGroup(QuadGroup group) { groups_.push_back(std::move(group)); setModified(true); }
    void clearGroups() { groups_.clear(); setModified(true); }

private:
    std::string name_;
    std::string filePath_;
    bool modified_ = false;
    ProjectSettings settings_;
    std::vector<Quad> quads_;
    std::vector<std::string> texturePaths_;  // Parallel to quads_
    std::vector<QuadGroup> groups_;
};

} // namespace quadmapper
