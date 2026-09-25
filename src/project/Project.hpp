#pragma once

#include "core/UUID.hpp"
#include "core/Types.hpp"
#include "ProjectSettings.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadGroup.hpp"
#include "scene/ProjectorCamera.hpp"
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

namespace quadmapper {

class Project {
public:
    Project();

    // ---- Multi-scene serialization DTOs (ADR-039 P5, .qmap v3) ----------------
    // The live multi-scene state lives in EditorState (Scene/QuadManager) and the
    // output config in OutputManager; these plain structs are the on-disk shape the
    // serializer round-trips. They deliberately avoid embedding the runtime Scene /
    // OutputWindow classes (no QuadManager / GLFW) so the DTO stays serialization-only.

    struct SceneViewData {
        Vec2 pan = Vec2(0.0f, 0.0f);
        float zoom = 1.0f;
        bool gridVisible = true;
        bool snapToGrid = false;
    };

    // A 3D object (.qmap v3.2, ADR-052). The mesh itself is not stored: it is re-imported
    // from path (absolute in memory; the serializer writes it relative to the project
    // directory when it can, as for media sources).
    struct ObjectData {
        UUID id;
        std::string name;
        std::string path;
        glm::mat4 transform{1.0f};  // model -> world
        TextureBinding texture;     // mediaId references the sources table
        bool visible = true;
        float opacity = 1.0f;
    };

    struct SceneData {
        UUID id;
        std::string name;
        UUID assignedOutputId;                 // null = edit-only (not projected)
        SceneViewData view;
        std::vector<Quad> quads;
        std::vector<std::string> texturePaths;  // parallel to quads
        std::vector<QuadGroup> groups;
        std::vector<ObjectData> objects;       // .qmap v3.2
    };

    struct OutputData {
        UUID id;
        std::string name;
        int monitor = -1;
        // Monitor identity (QM-087): name + virtual-desktop position of `monitor`
        // when saved, so a re-enumerated display still finds its projector.
        // Empty name = unknown (older files, or no monitor chosen). All three hold
        // the DESIRED monitor, never an automatic fallback (QM-090).
        std::string monitorName;
        IVec2 monitorPosition = IVec2(0, 0);
        IVec2 resolution = IVec2(1920, 1080);
        int rotationDegrees = 0;
        bool enabled = true;
        // The projector behind this output and the point pairs it was calibrated
        // from (.qmap v3.2, ADR-052).
        ProjectorCamera projector;
        std::vector<CalibrationPoint> calibrationPoints;
    };

    // Media sources bound to quads (.qmap v3.1 "sources" table, ADR-046). A quad
    // references one through TextureBinding::mediaId. In memory `path` is absolute;
    // the serializer writes it relative to the project directory when it can.
    struct TransportData {
        bool playing = false;
        LoopMode loopMode = LoopMode::LOOP;
        float speed = 1.0f;
    };

    struct SourceData {
        UUID id;
        std::string kind;            // "image" | "video" | "hap" | "procedural" | "camera"
        std::string path;            // image / video / hap
        std::string proceduralType;  // procedural: ProceduralFactory::getTypeName()
        int width = 1920;            // procedural
        int height = 1080;           // procedural
        std::string deviceName;      // camera
        std::optional<TransportData> transport;  // video / hap
    };

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

    // Multi-scene + output DTOs (.qmap v3). When non-empty the serializer writes
    // v3; the flat quads_/groups_ above stay populated as a backward-compatible
    // view (the union of all scenes' quads) so flat consumers keep working.
    std::vector<SceneData>& getScenes() { return scenes_; }
    const std::vector<SceneData>& getScenes() const { return scenes_; }
    std::vector<OutputData>& getOutputs() { return outputs_; }
    const std::vector<OutputData>& getOutputs() const { return outputs_; }
    std::vector<SourceData>& getSources() { return sources_; }
    const std::vector<SourceData>& getSources() const { return sources_; }

private:
    std::string name_;
    std::string filePath_;
    bool modified_ = false;
    ProjectSettings settings_;
    std::vector<Quad> quads_;
    std::vector<std::string> texturePaths_;  // Parallel to quads_
    std::vector<QuadGroup> groups_;
    std::vector<SceneData> scenes_;
    std::vector<OutputData> outputs_;
    std::vector<SourceData> sources_;
};

} // namespace quadmapper
