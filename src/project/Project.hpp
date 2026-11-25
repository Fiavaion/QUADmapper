#pragma once

#include "core/UUID.hpp"
#include "core/Types.hpp"
#include "ProjectSettings.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadGroup.hpp"
#include "scene/ProjectorCamera.hpp"
#include "scene/Slice.hpp"
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
        // Set on load when the mesh file is not there; path is kept so the file can be
        // relocated (PRD §4.1.8). Never written to disk.
        bool missing = false;
    };

    struct SceneData {
        UUID id;
        std::string name;
        IVec2 canvas = IVec2(1920, 1080);      // canvas size in pixels (.qmap v3.4, ADR-047)
        bool canvasLocked = false;             // the operator set the size (.qmap v3.4, Scene::isCanvasLocked)
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
        // The scene this output shows (null = none) and its slices of that scene's
        // canvas (.qmap v3.4, ADR-047). Older files are migrated on load: the scene
        // whose assignedOutputId named this output, one full-canvas slice.
        UUID sceneId;
        std::vector<Slice> slices;
    };

    // Media sources bound to quads (.qmap v3.1 "sources" table, ADR-046). A quad
    // references one through TextureBinding::mediaId. In memory `path` is absolute;
    // the serializer writes it relative to the project directory when it can.
    struct TransportData {
        bool playing = false;
        LoopMode loopMode = LoopMode::LOOP;
        float speed = 1.0f;
        bool timelineLocked = false;  // follows the master timeline (ADR-050)
        float volume = 1.0f;          // the source's sound, 0-1 (ADR-050 §2)
        bool muted = false;
    };

    // Show-wide sound (.qmap "audio", ADR-050 §5). The output device is a machine
    // preference, not part of the show.
    struct AudioData {
        float masterVolume = 1.0f;
        bool masterMuted = false;
    };

    struct SourceData {
        UUID id;
        std::string kind;            // "image" | "video" | "hap" | "procedural" | "camera" | "composition"
        std::string path;            // image / video / hap
        std::string proceduralType;  // procedural: ProceduralFactory::getTypeName()
        int width = 1920;            // procedural, composition
        int height = 1080;           // procedural, composition
        std::string deviceName;      // camera
        std::optional<TransportData> transport;  // video / hap
        // composition (ADR-061, .qmap 3.7): its cues name other entries of the table.
        struct CueData {
            UUID source;               // a "sources" id; null = black
            double holdSeconds = 5.0;
        };
        std::string name;              // composition
        std::string compositionMode = "timeline";  // "timeline" | "switch"
        std::vector<CueData> cues;
        double fadeSeconds = 0.5;
        bool loop = true;
        bool restartOnCue = true;
        // Set on load when a file source's file is not there. Its quads and objects
        // keep referencing it and path keeps the original location, so saving before
        // the file is relocated loses nothing (PRD §4.1.8). Never written to disk.
        bool missing = false;
    };

    // A MIDI mapping (.qmap "control": {"midi": [...]}, ADR-048). Plain strings so the
    // project module does not depend on src/control; the app converts.
    struct MidiMappingData {
        std::string device = "any";  // input device name, or "any"
        std::string type;            // "note" | "cc" | "program"
        int channel = 0;             // 1-16, 0 = any
        int number = 0;              // 0-127
        std::string target;          // control path, e.g. /qm/quad/<uuid>/opacity
        std::string mode;            // "trigger" | "toggle" | "momentary" | "value"
        int bits = 7;                // 14 = a 14-bit CC pair (cc, number 0-31)
    };

    const std::string& getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    const std::string& getFilePath() const { return filePath_; }
    void setFilePath(const std::string& path) { filePath_ = path; }

    ProjectSettings& getSettings() { return settings_; }
    const ProjectSettings& getSettings() const { return settings_; }

    // Quad management
    std::vector<Quad>& getQuads() { return quads_; }
    const std::vector<Quad>& getQuads() const { return quads_; }

    void addQuad(Quad quad) { quads_.push_back(std::move(quad)); }
    void clearQuads() { quads_.clear(); }

    // Texture path storage (relative paths for portability)
    std::vector<std::string>& getTexturePaths() { return texturePaths_; }
    const std::vector<std::string>& getTexturePaths() const { return texturePaths_; }

    // Group management for serialization
    std::vector<QuadGroup>& getGroups() { return groups_; }
    const std::vector<QuadGroup>& getGroups() const { return groups_; }

    void clearGroups() { groups_.clear(); }

    // Multi-scene + output DTOs (.qmap v3). When non-empty the serializer writes
    // v3; the flat quads_/groups_ above stay populated as a backward-compatible
    // view (the union of all scenes' quads) so flat consumers keep working.
    std::vector<SceneData>& getScenes() { return scenes_; }
    const std::vector<SceneData>& getScenes() const { return scenes_; }
    std::vector<OutputData>& getOutputs() { return outputs_; }
    const std::vector<OutputData>& getOutputs() const { return outputs_; }
    std::vector<SourceData>& getSources() { return sources_; }
    const std::vector<SourceData>& getSources() const { return sources_; }
    std::vector<MidiMappingData>& getMidiMappings() { return midiMappings_; }
    const std::vector<MidiMappingData>& getMidiMappings() const { return midiMappings_; }
    AudioData& getAudio() { return audio_; }
    const AudioData& getAudio() const { return audio_; }

private:
    std::string name_;
    std::string filePath_;
    ProjectSettings settings_;
    std::vector<Quad> quads_;
    std::vector<std::string> texturePaths_;  // Parallel to quads_
    std::vector<QuadGroup> groups_;
    std::vector<SceneData> scenes_;
    std::vector<OutputData> outputs_;
    std::vector<SourceData> sources_;
    std::vector<MidiMappingData> midiMappings_;
    AudioData audio_;
};

} // namespace quadmapper
