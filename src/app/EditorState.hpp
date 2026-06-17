#pragma once

// EditorState owns the per-session editor/scene state that used to live as ~40 function-
// static "globals" inside Application::render() (see ADR-031, TD-02). Promoting it to an
// owned Application member kills the hidden-global lifetime, removes the dual-track
// SceneManager/static-Scene split, and lets the output render path (TD-01) reach the scene
// without going through render()'s locals. render() aliases each field back to a same-named
// local reference, so the function body is unchanged by the extraction.

#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

#include <glad/glad.h>  // GLuint

#include "../core/Types.hpp"   // Vec2
#include "../core/UUID.hpp"
#include "../scene/Scene.hpp"
#include "../scene/QuadManager.hpp"
#include "../renderer/Texture.hpp"
#include "../video/VideoPlayer.hpp"
#include "../ui/panels/InspectorPanel.hpp"
#include "../ui/panels/MainMenuBar.hpp"
#include "../ui/panels/MediaBinPanel.hpp"
#include "../ui/panels/OutputPanel.hpp"
#include "../ui/panels/OutputPreviewPanel.hpp"
#include "../ui/panels/QuadListPanel.hpp"
#include "../ui/panels/ScenePanel.hpp"
#include "../ui/panels/VideoPlayerPanel.hpp"
#include "../ui/panels/ViewportEditor.hpp"

namespace quadmapper {

struct EditorState {
    // Context-sensitive selection tracking
    enum class SelectionType { None, Quad, Output, Media };

    // Scene + quad management (ADR-039 P1: multi-scene model).
    // The project holds one or more scenes; the editor always edits the ACTIVE
    // scene. activeScene()/quadManager() resolve it (and lazily guarantee one
    // scene exists, so the app can never be sceneless). Persistence of the full
    // scene list comes in a later phase; today one default scene is created.
    std::vector<std::unique_ptr<Scene>> scenes;
    UUID activeSceneId;

    Scene& activeScene() {
        if (scenes.empty()) {
            scenes.push_back(std::make_unique<Scene>());
            scenes.back()->setActive(true);
            activeSceneId = scenes.back()->getId();
        }
        for (auto& s : scenes) {
            if (s->getId() == activeSceneId) return *s;
        }
        activeSceneId = scenes.front()->getId();
        return *scenes.front();
    }
    QuadManager& quadManager() { return activeScene().getQuadManager(); }

    // ---- Scene-list management (ADR-039 P3) -------------------------------
    // Thin mutators over the scene list. The viewport, Quad List and Inspector
    // all read through activeScene()/quadManager(), so switching the active
    // scene makes them follow with no extra wiring. The proper testable
    // SceneManager returns in P5; these stay deliberately small until then.

    bool canDeleteScene() const { return scenes.size() > 1; }

    std::string generateUniqueSceneName() const {
        for (int n = static_cast<int>(scenes.size()) + 1; ; ++n) {
            std::string candidate = "Scene " + std::to_string(n);
            bool taken = false;
            for (const auto& s : scenes) {
                if (s && s->getName() == candidate) { taken = true; break; }
            }
            if (!taken) return candidate;
        }
    }

    Scene* addScene(const std::string& name) {
        auto scene = std::make_unique<Scene>();
        scene->setName(name);
        Scene* ptr = scene.get();
        scenes.push_back(std::move(scene));
        setActiveScene(ptr->getId());
        return ptr;
    }

    void setActiveScene(const UUID& id) {
        activeSceneId = id;
        for (auto& s : scenes) {
            if (s) s->setActive(s->getId() == id);
        }
        // Switching scenes changes the underlying QuadManager, so any vertex
        // selection / drag / insert state indexes a quad that is no longer the
        // active one. Drop it to avoid a stale index into another scene's quad.
        cornerHoveredVertex = cornerDraggedVertex = cornerSelectedVertex = -1;
        meshHoveredVertex = meshDraggedVertex = meshSelectedVertex = -1;
        snapActive = false;
        gridInsert = GridInsert::None;
        gridInsertMouseLatch = false;
        gridInsertDragIndex = -1;
    }

    bool deleteScene(const UUID& id) {
        if (scenes.size() <= 1) return false;  // never delete the last scene
        auto it = std::find_if(scenes.begin(), scenes.end(),
            [&](const std::unique_ptr<Scene>& s) { return s && s->getId() == id; });
        if (it == scenes.end()) return false;
        bool wasActive = (id == activeSceneId);
        scenes.erase(it);
        if (wasActive) setActiveScene(scenes.front()->getId());
        return true;
    }

    // Assign a scene to an output, enforcing one-scene-per-output (sceneForOutput
    // returns the first match, so a shared output would be ambiguous). Passing a
    // null outputId clears the assignment.
    void assignSceneToOutput(const UUID& sceneId, const UUID& outputId) {
        if (!outputId.isNull()) {
            for (auto& s : scenes) {
                if (s && s->getId() != sceneId && s->getAssignedOutputId() == outputId) {
                    s->setAssignedOutputId(UUID::null());
                }
            }
        }
        for (auto& s : scenes) {
            if (s && s->getId() == sceneId) { s->setAssignedOutputId(outputId); break; }
        }
    }

    std::unordered_map<UUID, std::unique_ptr<Texture>> quadTextures;
    std::unordered_map<UUID, std::string> quadTexturePaths;
    char currentTexturePath[512] = "";
    char projectPath[512] = "project.qmap";

    // UV test-grid toggle (#2): when on, newly-created quads get a colored UV
    // reference grid texture. seenQuadIds tracks which quads have already been
    // reconciled so the grid is applied once, at creation, across every code
    // path that adds a quad (default, new-project, the Quad List "+").
    bool uvGridOnNewQuads = false;
    std::unordered_set<UUID> seenQuadIds;
    char statusMessage[256] = "";
    float statusTimer = 0.0f;
    bool initialized = false;
    UUID dragDropQuadId;  // For drag-drop between groups

    // Corner editing state (CORNERS_ONLY mode - the default)
    int cornerHoveredVertex = -1;
    int cornerDraggedVertex = -1;
    int cornerSelectedVertex = -1;  // Persistent selection for keyboard control
    Vec2 cornerDragStartMousePos{0.0f};
    Vec2 cornerDragStartPos{0.0f};

    // Mesh editing state (MESH_EDIT mode)
    int meshHoveredVertex = -1;
    int meshDraggedVertex = -1;
    int meshSelectedVertex = -1;  // Persistent selection for keyboard control
    Vec2 meshDragStartMousePos{0.0f};

    // Snap indicator state (visual feedback when snapping)
    bool snapActive = false;
    Vec2 snapTargetPos{0.0f};
    Vec2 meshDragStartOffset{0.0f};

    // Video player state
    VideoPlayer videoPlayer;
    char videoPath[512] = "";
    GLuint videoTextureId = 0;
    int videoTextureWidth = 0;
    int videoTextureHeight = 0;
    UUID videoAssignedQuadId;  // Which quad has video assigned
    Texture videoTexWrapper;   // Wrapper for the raw video GL texture

    // Panels
    InspectorPanel inspectorPanel;
    MainMenuBar mainMenuBar;
    bool mainMenuBarInitialized = false;
    MediaBinPanel mediaBinPanel;
    bool mediaBinInitialized = false;
    OutputPanel outputPanel;
    bool outputPanelInitialized = false;
    QuadListPanel quadListPanel;
    ScenePanel scenePanel;
    OutputPreviewPanel outputPreviewPanel;
    VideoPlayerPanel videoPlayerPanel;
    ViewportEditor viewportEditor;  // Non-Panel: pre-NewFrame on-canvas warp surface (ADR-0034)

    // Panel visibility toggles
    bool showQuadList = true;
    bool showProperties = true;
    bool showResources = true;
    bool showVideoPlayer = true;

    // Current selection
    SelectionType currentSelectionType = SelectionType::Quad;

    // Grid-division insert tool (ADR-038 Tier 1 batch 3b). When armed, a click in
    // the viewport inserts a U (Column) or V (Row) division at the clicked param
    // position. gridInsertMouseLatch edge-detects so one click inserts once.
    enum class GridInsert { None, Column, Row };
    GridInsert gridInsert = GridInsert::None;
    bool gridInsertMouseLatch = false;
    int gridInsertDragIndex = -1;  // index of the just-inserted division being dragged
};

}  // namespace quadmapper
