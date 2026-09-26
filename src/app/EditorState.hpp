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
#include <array>
#include <algorithm>

#include <glad/glad.h>  // GLuint

#include "../core/Types.hpp"   // Vec2
#include "../core/UUID.hpp"
#include "../core/CommandHistory.hpp"
#include "../control/MidiMapping.hpp"
#include "../project/Project.hpp"
#include "../scene/CalibrationSession.hpp"
#include "../scene/Commands.hpp"
#include "../scene/Scene.hpp"
#include "../scene/QuadManager.hpp"
#include "../renderer/Texture.hpp"
#include "../renderer/UvTestGrid.hpp"
#include "../video/VideoPlayer.hpp"
#include "../ui/panels/CalibrationPanel.hpp"
#include "../ui/panels/ControlPanel.hpp"
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
    // quadResolver refers to this object's own `scenes`, so a copy or move would
    // leave it resolving against the source. EditorState lives in a unique_ptr.
    EditorState() = default;
    EditorState(const EditorState&) = delete;
    EditorState& operator=(const EditorState&) = delete;

    // Context-sensitive selection tracking
    enum class SelectionType { None, Quad, Output, Media, Object };

    // UI complexity mode (ADR-042). Simple = curated, friendly subset for new
    // users / students; Advanced = the full control set (today's UI). Panels read
    // this each frame to decide what to show. Default Simple; the startup value is
    // loaded from preferences in Application and persisted on toggle.
    enum class UiMode { Simple, Advanced };
    UiMode uiMode = UiMode::Simple;

    // Scene + quad management (ADR-039 P1: multi-scene model).
    // The project holds one or more scenes; the editor always edits the ACTIVE
    // scene. activeScene()/quadManager() resolve it (and lazily guarantee one
    // scene exists, so the app can never be sceneless). Persistence of the full
    // scene list comes in a later phase; today one default scene is created.
    std::vector<std::unique_ptr<Scene>> scenes;
    UUID activeSceneId;

    // Resolves undo-command targets by id against `scenes` (declared after it).
    SceneListResolver quadResolver{scenes};

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
        quadmapper::assignSceneToOutput(scenes, sceneId, outputId);
    }

    // Live-show take of scene `number` (1-9 keys, US-008; see planSceneTake). Returns
    // what was done; Kind::Route changed the routing (a saved project change).
    SceneTake takeScene(int number, const std::vector<SceneTakeOutput>& outputs) {
        const SceneTake take = planSceneTake(scenes, activeScene().getId(), number, outputs);
        if (take.kind == SceneTake::Kind::Route) assignSceneToOutput(take.sceneId, take.outputId);
        if (take.kind != SceneTake::Kind::None) setActiveScene(take.sceneId);
        return take;
    }

    // Duplicate a scene (Scene::duplicate) right after the original and make the copy
    // active. The copy's quads keep the source quads' saved media paths and UV test
    // grids, and count as already seen so the new-quad UV grid does not replace their
    // media. Main thread only (the UV grid is a GL texture).
    Scene* duplicateScene(const UUID& id) {
        const int index = findSceneIndex(scenes, id);
        if (index < 0) return nullptr;
        std::unordered_map<UUID, UUID> quadIdMap;
        std::unique_ptr<Scene> copy = scenes[static_cast<size_t>(index)]->duplicate(&quadIdMap);
        copy->setName(uniqueSceneCopyName(scenes, scenes[static_cast<size_t>(index)]->getName()));
        for (const auto& [sourceId, copyId] : quadIdMap) {
            seenQuadIds.insert(copyId);
            auto pathIt = quadTexturePaths.find(sourceId);
            if (pathIt != quadTexturePaths.end()) {
                std::string path = pathIt->second;
                quadTexturePaths[copyId] = std::move(path);
            }
            if (quadTextures.count(sourceId)) {
                assignUvTestGrid(quadTextures, quadTexturePaths, copyId, 1024, 8);
            }
        }
        Scene* ptr = copy.get();
        scenes.insert(scenes.begin() + index + 1, std::move(copy));
        setActiveScene(ptr->getId());
        return ptr;
    }

    // Reorder the scene list (the saved order and the 1-9 numbering).
    bool moveScene(size_t from, size_t to) { return quadmapper::moveScene(scenes, from, to); }

    std::unordered_map<UUID, std::unique_ptr<Texture>> quadTextures;
    std::unordered_map<UUID, std::string> quadTexturePaths;
    char currentTexturePath[512] = "";
    char projectPath[512] = "";  // empty = untitled: Save asks for a path (ADR-046)

    // ---- Show-safe file actions (ADR-046 §4) --------------------------------
    // The File menu, Ctrl+S, the Project tab and the window close button only REQUEST
    // an action; Application runs it at the next frame boundary, before render()
    // takes any scene/QuadManager references. New/Open/Load/Exit first pass the
    // unsaved-changes check, which may raise the Save / Don't Save / Cancel modal.
    enum class FileAction { None, New, Open, Load, Save, SaveAs, Exit, RestoreRecovery, DiscardRecovery,
                            ImportObject };
    FileAction pendingFileAction = FileAction::None;
    FileAction unsavedPromptFor = FileAction::None;  // != None: unsaved-changes modal is up
    bool saveBeforeAction = false;   // modal answered "Save"
    bool discardConfirmed = false;   // modal answered "Don't Save"
    // The file a queued Load opens, captured when it is requested (the Project tab's
    // Load button or the Open dialog). Kept apart from projectPath, which a save
    // rewrites, so "Save" at the unsaved prompt still loads the show that was asked for.
    std::string pendingLoadPath;
    // Playing sources paused because only the undo history still holds them (their
    // quad was deleted); resumed if undo puts them back on screen.
    std::unordered_set<UUID> sourcesPausedWhileUnseen;
    std::string recoveryFilePath;    // != "": "recover unsaved work" modal is up
    std::string recoveryProjectPath; // project that recovery file belongs to ("" = untitled)

    // ---- Missing media (PRD §4.1.8, see MissingMedia.hpp) ----------------------
    // File sources that could not be opened when the project loaded, by the id their
    // quads and objects keep referencing. Saving writes them back as they were; the
    // Missing Media window relocates them. A 3D object's missing mesh needs no entry:
    // it is an object with a mesh path and no mesh.
    std::unordered_map<UUID, Project::SourceData> missingSources;
    bool showMissingMedia = false;  // the Missing Media window is open
    std::unordered_map<std::string, std::string> missingMediaErrors;  // path -> last relocate error
    // Drawn in place of a missing source (MissingMediaLook.hpp), created once on first
    // use: the checker in the editor and previews, black on projector outputs.
    std::unique_ptr<Texture> missingPatternTexture;
    std::unique_ptr<Texture> missingBlackTexture;

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

    // Bezier edge-handle editing (ADR-043). Indices 0..7 = control(edge,which);
    // bezierDragBefore snapshots all offsets at press for one-step undo on release.
    int bezierHovered = -1;
    int bezierDragged = -1;
    std::array<Vec2, 8> bezierDragBefore{};

    // Video player state (ADR-040 P3): video is a SourceManager VideoFileSource.
    // The Video Player panel drives this active source; quads bind it by mediaId,
    // so one decode can feed many quads (the box-wrap). No per-quad video texture
    // and no single videoAssignedQuadId anymore.
    char videoPath[512] = "";
    UUID activeVideoSourceId;

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
    CalibrationPanel calibrationPanel;  // shown while calibrating a projector (ADR-052)
    ControlPanel controlPanel;          // OSC / MIDI external control (ADR-048)

    // The show's MIDI mappings (ADR-048 §4), saved in the project. Their targets name
    // quads and sources by live id; load remaps the saved ids.
    std::vector<control::MidiMapping> midiMappings;

    // Panel visibility toggles
    bool showQuadList = true;
    bool showProperties = true;
    bool showResources = true;
    bool showVideoPlayer = true;
    bool showControl = false;  // Window > Control (OSC / MIDI)

    // Current selection
    SelectionType currentSelectionType = SelectionType::Quad;
    // The 3D object of the active scene the Inspector edits (ADR-052); null = none.
    UUID selectedObjectId;

    // Projector calibration mode (ADR-052 §6). While an output is being calibrated the
    // canvas edits that output's calibration points instead of quad vertices.
    UUID calibratingOutputId;  // null = not calibrating
    CalibrationSession calibration;

    // Enter calibration on an output. The scene routed to it becomes the active one,
    // so the canvas shows that projector.
    void beginCalibration(const UUID& outputId, const ProjectorCamera& projector) {
        for (auto& s : scenes) {
            if (s && s->getAssignedOutputId() == outputId) { setActiveScene(s->getId()); break; }
        }
        calibratingOutputId = outputId;
        calibration.begin(projector);
    }
    void endCalibration() {
        calibratingOutputId = UUID::null();
        calibration.cancelPick();
    }

    // Grid-division insert tool (ADR-038 Tier 1 batch 3b). When armed, a click in
    // the viewport inserts a U (Column) or V (Row) division at the clicked param
    // position. gridInsertMouseLatch edge-detects so one click inserts once.
    enum class GridInsert { None, Column, Row };
    GridInsert gridInsert = GridInsert::None;
    bool gridInsertMouseLatch = false;
    int gridInsertDragIndex = -1;  // index of the just-inserted division being dragged

    // Grid snapshot captured at the press that begins a click-to-insert gesture, so
    // the whole insert+drag commits as ONE undo step on release (ADR-043).
    std::vector<float> gridInsertBeforeU;
    std::vector<float> gridInsertBeforeV;
    std::vector<Vec2> gridInsertBeforeOffsets;

    // ---- Viewport view transform (ADR-041, session-only) ------------------
    // The editor canvas is drawn into the dockspace central node with the
    // projector's aspect, scaled by canvasZoom and translated by canvasPan.
    // canvasRegion* is that node's rect in LOGICAL window coords (ImGui/GLFW
    // screen space, matching glfwGetCursorPos), captured the previous frame in
    // Application::render() after the DockSpace is laid out. lastView* are the
    // (scale, offset) ViewportEditor computed last frame in viewport-normalized
    // [0,1] space; the post-NewFrame zoom/pan input reuses them for cursor-
    // anchored zoom. Nothing here is persisted to .qmap.
    float canvasZoom = 1.0f;
    Vec2 canvasPan{0.0f, 0.0f};
    Vec2 canvasRegionPos{0.0f, 0.0f};
    Vec2 canvasRegionSize{0.0f, 0.0f};
    Vec2 lastViewScale{1.0f, 1.0f};
    Vec2 lastViewOffset{0.0f, 0.0f};

    // Left-button state last frame, for click-to-select edge detection in the
    // viewport (ADR-042). Distinct from the corner/mesh drag latches.
    bool canvasMouseDownPrev = false;

    // Undo/redo for quad edits, deletes and duplicates. Commands hold (scene id, quad
    // id) and resolve through quadResolver, so a quad or scene destroyed while its
    // commands are queued makes those steps safe no-ops.
    CommandHistory commandHistory;

    // Stable command target for a quad of the active scene.
    QuadRef quadRef(const Quad& quad) {
        return QuadRef{&quadResolver, activeScene().getId(), quad.getId()};
    }

    // Delete every selected quad of the active scene as ONE undo step, through
    // Scene::deleteQuad (group cleanup). Legacy quadTextures/quadTexturePaths entries
    // are kept so undo brings the quad back with its UV grid and saved media path.
    void deleteSelectedQuads() {
        Scene& scene = activeScene();
        const std::vector<UUID> ids(scene.getQuadManager().getSelectedIds().begin(),
                                    scene.getQuadManager().getSelectedIds().end());
        if (ids.empty()) return;
        commandHistory.beginCompoundCommand(ids.size() == 1 ? "Delete quad" : "Delete quads");
        for (const UUID& id : ids) {
            commandHistory.execute(std::make_unique<DeleteQuadCommand>(
                QuadRef{&quadResolver, scene.getId(), id}));
        }
        commandHistory.endCompoundCommand();
    }
};

}  // namespace quadmapper
