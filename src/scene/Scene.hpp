#pragma once

#include "core/Types.hpp"
#include "core/UUID.hpp"
#include "Quad.hpp"
#include "QuadGroup.hpp"
#include "QuadManager.hpp"
#include "MeshObject.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace quadmapper {

// View state for a scene (pan, zoom, grid settings)
struct SceneViewState {
    Vec2 panOffset = Vec2(0.0f, 0.0f);
    float zoomLevel = 1.0f;
    bool gridVisible = true;
    bool snapToGrid = false;
};

// A canvas when nothing gives another size (ADR-047 §1), and the largest allowed:
// 8192, the texture size every GL 3.3 (Direct3D 10 class) GPU supports, so a canvas
// texture (256 MB at the limit) can always be allocated.
constexpr int DEFAULT_CANVAS_WIDTH = 1920;
constexpr int DEFAULT_CANVAS_HEIGHT = 1080;
constexpr int MAX_CANVAS_DIMENSION = 8192;

class Scene {
public:
    Scene();
    explicit Scene(const UUID& id);

    const UUID& getId() const { return id_; }
    const std::string& getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    bool isActive() const { return isActive_; }
    void setActive(bool active) { isActive_ = active; }

    // The scene's canvas in pixels (ADR-047 §1). Quads and objects stay normalized
    // [0, 1] in canvas space; outputs show slices of the canvas (OutputWindow). Each
    // side is clamped to [1, MAX_CANVAS_DIMENSION].
    IVec2 getCanvasSize() const { return canvasSize_; }
    void setCanvasSize(IVec2 size);

    // True once the operator has set the canvas size (Scenes panel): routing and
    // output size changes then keep it, instead of the canvas of a lone projector's
    // default slice adopting that projector's resolution (ADR-047 Amendment 3).
    bool isCanvasLocked() const { return canvasLocked_; }
    void setCanvasLocked(bool locked) { canvasLocked_ = locked; }

    // View state access
    SceneViewState& getViewState() { return viewState_; }
    const SceneViewState& getViewState() const { return viewState_; }
    void resetView();

    // QuadManager access
    QuadManager& getQuadManager() { return quadManager_; }
    const QuadManager& getQuadManager() const { return quadManager_; }

    // Convenience methods that delegate to QuadManager
    Quad* createQuad();
    void deleteQuad(const UUID& id);
    Quad* getQuad(const UUID& id);
    const std::vector<std::unique_ptr<Quad>>& getQuads() const { return quadManager_.getQuads(); }
    std::vector<Quad*> getQuadsByZOrder() const { return quadManager_.getQuadsByZOrder(); }

    // ============ Group Management API ============

    // Group creation and deletion
    QuadGroup* createGroup(const std::string& name = "New Group");
    void deleteGroup(const UUID& id);
    void clearGroups();

    // Group access
    QuadGroup* getGroup(const UUID& id);
    const QuadGroup* getGroup(const UUID& id) const;
    const std::vector<std::unique_ptr<QuadGroup>>& getGroups() const { return groups_; }
    size_t getGroupCount() const { return groups_.size(); }

    // Group membership management
    void addQuadToGroup(const UUID& groupId, const UUID& quadId);
    void removeQuadFromGroup(const UUID& groupId, const UUID& quadId);
    std::vector<QuadGroup*> getGroupsForQuad(const UUID& quadId);
    std::vector<const QuadGroup*> getGroupsForQuad(const UUID& quadId) const;
    bool isQuadInGroup(const UUID& groupId, const UUID& quadId) const;

    // Create group from current selection in QuadManager
    QuadGroup* createGroupFromSelection(const std::string& name = "New Group");

    // Apply group visibility/lock to all member quads
    void applyGroupVisibility(const UUID& groupId, bool visible);
    void applyGroupLock(const UUID& groupId, bool locked);

    // Generate unique group name
    std::string generateUniqueGroupName() const;

    // 3D objects (ADR-052): imported meshes, drawn by each output from its projector.
    MeshObject* createObject(const std::string& name = "Object");
    MeshObject* adoptObject(std::unique_ptr<MeshObject> object);  // e.g. from a loader
    void deleteObject(const UUID& id);
    // Remove an object and hand it over whole (undo keeps it); `index` receives its
    // list position. nullptr when there is no such object.
    std::unique_ptr<MeshObject> takeObject(const UUID& id, size_t* index = nullptr);
    // Put an object in at `index` (clamped to the end of the list). nullptr, and the
    // object is dropped, when it is null or its id is already in the scene.
    MeshObject* insertObject(std::unique_ptr<MeshObject> object, size_t index);
    MeshObject* getObject(const UUID& id);
    const std::vector<std::unique_ptr<MeshObject>>& getObjects() const { return objects_; }
    // World bounds of every visible object with a mesh; false when there are none.
    bool visibleObjectBounds(Vec3& min, Vec3& max) const;

    // Deep copy under a fresh scene id (duplicate scene). Quads, groups and 3D objects
    // get fresh ids; group membership and parents follow the new ids; texture bindings
    // keep their media ids, so the copy shares its sources (ADR-040). The copy keeps the
    // name, view and canvas size (and its lock), is inactive and no output shows it. quadIdMap, when
    // given, receives source quad id -> copy quad id.
    std::unique_ptr<Scene> duplicate(std::unordered_map<UUID, UUID>* quadIdMap = nullptr) const;

private:
    UUID id_;
    std::string name_;
    bool isActive_ = false;
    IVec2 canvasSize_ = IVec2(DEFAULT_CANVAS_WIDTH, DEFAULT_CANVAS_HEIGHT);
    bool canvasLocked_ = false;
    SceneViewState viewState_;
    QuadManager quadManager_;
    std::vector<std::unique_ptr<QuadGroup>> groups_;
    int nextGroupNumber_ = 1;
    std::vector<std::unique_ptr<MeshObject>> objects_;
};

// ---- Scene-list operations (ADR-039) ----------------------------------------
// The project's scenes in list order; the order is the saved order and the 1-9
// hotkey numbering.
using SceneList = std::vector<std::unique_ptr<Scene>>;

// Position of the scene with this id, or -1.
int findSceneIndex(const SceneList& scenes, const UUID& sceneId);

// "<baseName> copy", else "<baseName> copy 2", "... copy 3": the first no scene uses.
std::string uniqueSceneCopyName(const SceneList& scenes, const std::string& baseName);

// Move the scene at `from` to position `to`; the others keep their relative order.
// False (and no change) when either index is out of range or they are equal.
bool moveScene(SceneList& scenes, size_t from, size_t to);

// Live-show scene take (US-008, ADR-047 §5): what pressing number key N does.
// An output and the scene it shows (null = none).
struct SceneTakeOutput {
    UUID id;
    bool enabled = true;
    UUID sceneId;
};

struct SceneTake {
    enum class Kind {
        None,      // nothing to do (out of range, or scene N is already active and live)
        Activate,  // make scene N active; routing unchanged
        Route      // show scene N on every output in outputIds, make N active
    };
    Kind kind = Kind::None;
    UUID sceneId;
    std::vector<UUID> outputIds;
};

// Decide the take for scene `number` (1-based, list order). If an output in `outputs`
// already shows scene N, it just becomes active. Otherwise it goes on every output that
// shows the active scene, so a blended group of projectors changes together; when no
// output shows the active scene, on the first enabled output and every output showing
// the same scene as it (its group); with no enabled output, N just becomes active.
SceneTake planSceneTake(const SceneList& scenes, const UUID& activeSceneId, int number,
                        const std::vector<SceneTakeOutput>& outputs);

} // namespace quadmapper
