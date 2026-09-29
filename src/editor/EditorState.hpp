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
#include "GridUndo.hpp"
#include "PerformLock.hpp"
#include "../project/Project.hpp"
#include "../output/OutputManager.hpp"
#include "../output/OutputWindow.hpp"
#include "../scene/CalibrationSession.hpp"
#include "../scene/Commands.hpp"
#include "../scene/ObjLoader.hpp"
#include "../scene/Scene.hpp"
#include "../scene/QuadManager.hpp"
#include "../renderer/Texture.hpp"
#include "../renderer/UvTestGrid.hpp"
#include "../video/VideoPlayer.hpp"

namespace quadmapper {

// Resolves output undo steps by output id against the live outputs: calibration steps
// (CalibrationEditCommand, TD-17) get the output's points and projector, plus the
// editor's session while that output is the one being calibrated; slice steps
// (SliceEditCommand, ADR-047) get its slices. A removed or re-created output (new id)
// resolves to nothing.
class OutputCalibrationResolver : public OutputResolver {
public:
    OutputCalibrationResolver(const UUID& calibratingOutputId, CalibrationSession& session)
        : calibratingOutputId_(calibratingOutputId), session_(session) {}

    void setOutputManager(OutputManager* outputs) { outputs_ = outputs; }

    CalibrationTarget findCalibration(const UUID& outputId) override {
        OutputWindow* output = outputs_ && !outputId.isNull() ? outputs_->getOutput(outputId) : nullptr;
        if (!output) return {};
        return {&output->getCalibrationPoints(), &output->getProjector(),
                calibratingOutputId_ == outputId ? &session_ : nullptr};
    }

    bool readSlices(const UUID& outputId, OutputSlices& out) override {
        const OutputWindow* output = outputs_ && !outputId.isNull() ? outputs_->getOutput(outputId) : nullptr;
        if (!output) return false;
        out.slices = output->getSlices();
        out.canvas = output->getSlicesCanvas();
        return true;
    }

    bool writeSlices(const UUID& outputId, const OutputSlices& slices) override {
        OutputWindow* output = outputs_ && !outputId.isNull() ? outputs_->getOutput(outputId) : nullptr;
        if (!output) return false;
        output->getSlices() = slices.slices;
        output->setSlicesCanvas(slices.canvas);
        return true;
    }

    void rescaleSceneSlices(const UUID& sceneId, IVec2 from, IVec2 to) override {
        if (!outputs_ || sceneId.isNull()) return;
        for (const auto& output : outputs_->getOutputs()) {
            if (!output || output->getSceneId() != sceneId) continue;
            const IVec2 made = output->getSlicesCanvas();
            rescaleSlices(output->getSlices(), (made.x > 0 && made.y > 0) ? made : from, to);
            output->setSlicesCanvas(to);
        }
    }

private:
    const UUID& calibratingOutputId_;
    CalibrationSession& session_;
    OutputManager* outputs_ = nullptr;
};

// Merge key for moves of one warp corner of one slice (keyboard, mouse or the
// Inspector's fields): all of them are one target.
inline std::string sliceWarpCornerKey(const UUID& sliceId, int corner) {
    return "warp " + sliceId.toString() + " " + std::to_string(corner);
}

// Bookkeeping for the meshes EditorState::objLoads is parsing (TD-16a, MeshLoads.hpp).
struct MeshLoadState {
    std::unordered_map<std::string, uint64_t> tickets;    // mesh path -> its parse
    std::unordered_map<std::string, std::string> errors;  // mesh path -> why it did not load
    // Missing meshes being relocated: the new file's path -> the missing paths whose
    // objects move to it once it has parsed (Missing Media "Locate..." / search).
    std::unordered_map<std::string, std::vector<std::string>> relocations;
    // Imported objects whose import completes (becomes an undo step) once their mesh
    // has loaded.
    struct Import {
        UUID sceneId;
        UUID objectId;
    };
    std::vector<Import> imports;
};

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
        meshSelectedHandle = meshHoveredHandle = meshDraggedHandle = -1;
        snapActive = false;
        gridInsert = GridInsert::None;
        gridInsertMouseLatch = false;
        gridInsertDragIndex = -1;
    }

    // Delete a scene. Outputs showing it (in `outputs`, when given) then show no
    // scene; they keep their slices for the next scene routed to them.
    bool deleteScene(const UUID& id, OutputManager* outputs) {
        if (scenes.size() <= 1) return false;  // never delete the last scene
        auto it = std::find_if(scenes.begin(), scenes.end(),
            [&](const std::unique_ptr<Scene>& s) { return s && s->getId() == id; });
        if (it == scenes.end()) return false;
        if (outputs) {
            outputs->cancelSceneFades(id);
            for (const auto& output : outputs->getOutputs()) {
                if (output && output->getSceneId() == id) {
                    OutputManager::showScene(*output, nullptr, scenes);
                }
            }
        }
        bool wasActive = (id == activeSceneId);
        scenes.erase(it);
        if (wasActive) setActiveScene(scenes.front()->getId());
        return true;
    }

    // Live-show take of scene `number` (1-9 keys, US-008, ADR-047 §5; see
    // planSceneTake). Returns what was done; Kind::Route changed the routing of every
    // output in take.outputIds (a saved project change), each fading from what it
    // showed over `fadeSeconds` (0 = a cut; OutputManager::takeScene).
    SceneTake takeScene(int number, OutputManager& outputs, double fadeSeconds = 0.0) {
        std::vector<SceneTakeOutput> routing;
        for (const auto& output : outputs.getOutputs()) {
            if (!output) continue;
            const Scene* shown = OutputManager::sceneForOutput(scenes, *output);
            routing.push_back({output->getId(), output->isEnabled(),
                               shown ? shown->getId() : UUID::null()});
        }
        const SceneTake take = planSceneTake(scenes, activeScene().getId(), number, routing);
        if (take.kind == SceneTake::Kind::Route) {
            Scene* target = scenes[static_cast<size_t>(findSceneIndex(scenes, take.sceneId))].get();
            for (const UUID& id : take.outputIds) {
                if (OutputWindow* output = outputs.getOutput(id)) {
                    outputs.takeScene(*output, target, scenes, fadeSeconds);
                }
            }
        }
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
    // How each clip was playing when releaseUnusedSources released it, by its old id,
    // so a binding that comes back to it re-opens it as it was.
    std::unordered_map<UUID, Project::TransportData> releasedTransports;
    std::string recoveryFilePath;    // != "": "recover unsaved work" modal is up
    std::string recoveryProjectPath; // project that recovery file belongs to ("" = untitled)

    // ---- Missing media (PRD §4.1.8, see MissingMedia.hpp) ----------------------
    // File sources that could not be opened when the project loaded, by the id their
    // quads and objects keep referencing. Saving writes them back as they were; the
    // Missing Media window relocates them. A 3D object's missing mesh needs no entry:
    // it is an object with a mesh path and no mesh.
    std::unordered_map<UUID, Project::SourceData> missingSources;
    std::unordered_map<std::string, std::string> missingMediaErrors;  // path -> last relocate error
    // Drawn in place of a missing source (MissingMediaLook.hpp), created once on first
    // use: the checker in the editor and previews, black on projector outputs.
    std::unique_ptr<Texture> missingPatternTexture;
    std::unique_ptr<Texture> missingBlackTexture;

    // ---- 3D object meshes parsed off the render thread (TD-16a, MeshLoads.hpp) --
    ObjLoadQueue objLoads;
    MeshLoadState meshLoads;

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

    // Grid tangent handles of the selected mesh vertex (ADR-049 §3): GridHandle
    // values, -1 for none. meshSelectedHandle is the keyboard target (H cycles it);
    // a drag snapshots the vertex's tangents for one undo step on release.
    int meshSelectedHandle = -1;
    int meshHoveredHandle = -1;
    int meshDraggedHandle = -1;
    Vec2 meshHandleDragStartMousePos{0.0f};
    Vec2 meshHandleDragStartPos{0.0f};
    VertexTangents meshHandleDragBefore;

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

    // The show's MIDI mappings (ADR-048 §4), saved in the project. Their targets name
    // quads and sources by live id; load remaps the saved ids.
    std::vector<control::MidiMapping> midiMappings;

    // Windows editor code wants opened, by panel id (the Missing Media window after a
    // load, Preferences on Ctrl+,). The panels own their visibility; the ui's PanelHost
    // drains these once a frame (ADR-051 §2).
    std::vector<std::string> panelOpenRequests;
    void requestPanelOpen(std::string panelId) { panelOpenRequests.push_back(std::move(panelId)); }
    // The Missing Media window's panel id (its ImGui window name). It closes itself
    // once nothing is missing.
    static constexpr const char* MISSING_MEDIA_PANEL_ID = "Missing Media##relocate";

    // Current selection
    SelectionType currentSelectionType = SelectionType::Quad;
    // The 3D object of the active scene the Inspector edits (ADR-052); null = none.
    UUID selectedObjectId;
    // The media bin item and the Outputs tab output the Inspector shows; null = none.
    // The panels write them here (ADR-051 §1), so editor code never touches a panel.
    UUID selectedMediaId;
    UUID selectedOutputId;

    // The media bin's hover-to-play preview: one open clip (previewSourceId, playing
    // previewSourcePath) that no quad is bound to, so the unused-source cleanup keeps
    // it. The bin never removes a preview itself, since an assignment earlier in the
    // same frame may have bound it: a preview whose hover ended goes to endedPreviews,
    // and the frame boundary (releaseUnusedSources) removes each one nothing bound.
    UUID previewSourceId;
    std::string previewSourcePath;
    std::vector<UUID> endedPreviews;

    // Projector calibration mode (ADR-052 §6). While an output is being calibrated the
    // canvas edits that output's calibration points instead of quad vertices.
    UUID calibratingOutputId;  // null = not calibrating
    CalibrationSession calibration;
    // Calibration undo steps resolve through this (declared after what it refers to).
    // Application gives it the OutputManager.
    OutputCalibrationResolver outputResolver{calibratingOutputId, calibration};
    OutputRef outputRef(const UUID& outputId) { return OutputRef{&outputResolver, outputId}; }

    // Enter calibration on an output, however it is sliced. The scene it shows becomes
    // the active one, so the canvas shows that projector's output frame, slices, warp
    // and blend included, with the calibration points in projector pixels
    // (calibrationViewOutput, TD-29). Leaves slice warp editing.
    // Not under the perform lock (ADR-051 §4).
    void beginCalibration(const OutputWindow& output) {
        if (performLock.isOn()) return;
        endSliceWarp();
        if (findSceneIndex(scenes, output.getSceneId()) >= 0) setActiveScene(output.getSceneId());
        calibratingOutputId = output.getId();
        calibration.begin(output.getCalibrationPoints(), output.getProjector());
    }
    void endCalibration() {
        calibratingOutputId = UUID::null();
        calibration.cancelPick();
    }

    // ---- Slice editing (ADR-047 part B, SliceEditing.hpp) ------------------------
    // The slice the Inspector's slice editor shows for the selected output.
    UUID selectedSliceId;
    // Slice warp editing: while sliceWarpOutputId is set, the canvas shows that output's
    // frame and the mouse and the canvas keys (Tab / arrows / Shift / Alt) move the
    // corners of its selected slice's warp; quad editing is suspended, as it is while
    // calibrating. Esc or the slice editor's toggle leaves it.
    UUID sliceWarpOutputId;
    int sliceWarpSelectedCorner = -1;
    int sliceWarpHoveredCorner = -1;
    int sliceWarpDraggedCorner = -1;
    Vec2 sliceWarpDragStartMouse{0.0f};
    Vec2 sliceWarpDragStartPos{0.0f};
    OutputSlices sliceWarpDragBefore;  // for one undo step when the drag ends

    // Start editing the warp of `sliceId` on `output` on the canvas. Not while
    // calibrating (calibration owns the canvas then), nor under the perform lock.
    void beginSliceWarp(const OutputWindow& output, const UUID& sliceId) {
        if (!calibratingOutputId.isNull() || performLock.isOn()) return;
        finishSliceWarpDrag();
        sliceWarpOutputId = output.getId();
        selectedSliceId = sliceId;
        sliceWarpSelectedCorner = sliceWarpHoveredCorner = sliceWarpDraggedCorner = -1;
    }
    // Leaving warp editing (Esc, the slice editor's button, calibration) with a corner
    // drag in progress keeps the corner where the operator put it, as one undo step.
    void endSliceWarp() {
        finishSliceWarpDrag();
        sliceWarpOutputId = UUID::null();
        sliceWarpSelectedCorner = sliceWarpHoveredCorner = sliceWarpDraggedCorner = -1;
    }

    // End the corner drag in progress, if any (the mouse released, or warp editing
    // left mid-drag): a corner that moved becomes one undo step from
    // sliceWarpDragBefore, merged with the corner's other moves. Nothing is recorded
    // when the output or the slice is gone.
    void finishSliceWarpDrag() {
        const int corner = sliceWarpDraggedCorner;
        if (corner < 0 || corner >= 4) return;
        sliceWarpDraggedCorner = -1;
        sliceWarpSelectedCorner = corner;
        OutputSlices now;
        if (!outputResolver.readSlices(sliceWarpOutputId, now)) return;
        const auto slice = std::find_if(now.slices.begin(), now.slices.end(),
                                        [&](const Slice& s) { return s.id == selectedSliceId; });
        if (slice == now.slices.end() ||
            slice->warp.getCorners()[static_cast<size_t>(corner)] == sliceWarpDragStartPos) {
            return;
        }
        commandHistory.execute(std::make_unique<SliceEditCommand>(
            outputRef(sliceWarpOutputId), "Move slice warp corner",
            sliceWarpCornerKey(selectedSliceId, corner), sliceWarpDragBefore, std::move(now)));
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
    GridSnapshot gridInsertBefore;

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

    // The perform lock (ADR-051 §4, PerformLock.hpp): runtime only, never saved. While
    // it is on, commandHistory is read-only.
    PerformLock performLock;

    // Undo/redo for quad and 3D-object edits, deletes and duplicates. Commands hold
    // (scene id, quad or object id) and resolve through quadResolver, so a target or
    // scene destroyed while its commands are queued makes those steps safe no-ops.
    CommandHistory commandHistory;

    // Stable command target for a quad of the active scene.
    QuadRef quadRef(const Quad& quad) {
        return QuadRef{&quadResolver, activeScene().getId(), quad.getId()};
    }

    // Stable command target for a 3D object of the active scene (TD-16).
    ObjectRef objectRef(const MeshObject& object) {
        return ObjectRef{&quadResolver, activeScene().getId(), object.getId()};
    }

    // Delete key on a selected 3D object (the Object selection of the active scene), as
    // an undo step. False when no object is selected, or a quad is (selecting an object
    // deselects the quads, so a quad selection is the newer one).
    bool deleteSelectedObject() {
        if (currentSelectionType != SelectionType::Object || selectedObjectId.isNull()) return false;
        if (activeScene().getQuadManager().hasSelection()) return false;
        Scene& scene = activeScene();
        if (!scene.getObject(selectedObjectId)) return false;
        commandHistory.execute(std::make_unique<DeleteObjectCommand>(
            ObjectRef{&quadResolver, scene.getId(), selectedObjectId}));
        selectedObjectId = UUID();
        return true;
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
