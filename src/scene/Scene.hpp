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

class Scene {
public:
    Scene();
    explicit Scene(const UUID& id);

    const UUID& getId() const { return id_; }
    const std::string& getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    bool isActive() const { return isActive_; }
    void setActive(bool active) { isActive_ = active; }

    // Output routing (ADR-039): the output window this scene projects to. At most
    // one output per scene; null means the scene is edit-only (not projected).
    const UUID& getAssignedOutputId() const { return assignedOutputId_; }
    void setAssignedOutputId(const UUID& id) { assignedOutputId_ = id; }
    bool hasAssignedOutput() const { return !assignedOutputId_.isNull(); }

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
    MeshObject* getObject(const UUID& id);
    const std::vector<std::unique_ptr<MeshObject>>& getObjects() const { return objects_; }
    // World bounds of every visible object with a mesh; false when there are none.
    bool visibleObjectBounds(Vec3& min, Vec3& max) const;

    // Deep copy under a fresh scene id (duplicate scene). Quads, groups and 3D objects
    // get fresh ids; group membership and parents follow the new ids; texture bindings
    // keep their media ids, so the copy shares its sources (ADR-040). The copy keeps the
    // name and view, is inactive and is not assigned to an output. quadIdMap, when
    // given, receives source quad id -> copy quad id.
    std::unique_ptr<Scene> duplicate(std::unordered_map<UUID, UUID>* quadIdMap = nullptr) const;

private:
    UUID id_;
    std::string name_;
    bool isActive_ = false;
    UUID assignedOutputId_;  // output this scene projects to (null = edit-only)
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

// Route a scene to an output. One scene per output: any other scene on that output is
// unassigned. A null outputId clears the scene's assignment.
void assignSceneToOutput(SceneList& scenes, const UUID& sceneId, const UUID& outputId);

// Unassign every scene routed to this output (it was removed). True if any was.
bool clearOutputAssignments(SceneList& scenes, const UUID& outputId);

// "<baseName> copy", else "<baseName> copy 2", "... copy 3": the first no scene uses.
std::string uniqueSceneCopyName(const SceneList& scenes, const std::string& baseName);

// Move the scene at `from` to position `to`; the others keep their relative order.
// False (and no change) when either index is out of range or they are equal.
bool moveScene(SceneList& scenes, size_t from, size_t to);

// Live-show scene take (US-008): what pressing number key N does.
struct SceneTakeOutput {
    UUID id;
    bool enabled = true;
};

struct SceneTake {
    enum class Kind {
        None,      // nothing to do (out of range, or scene N is already active and live)
        Activate,  // make scene N active; routing unchanged
        Route      // route scene N to outputId (taking it from its scene), make N active
    };
    Kind kind = Kind::None;
    UUID sceneId;
    UUID outputId;
};

// Decide the take for scene `number` (1-based, list order). An assignment to an output
// that is not in `outputs` (deleted) counts as no assignment. If scene N is assigned,
// it just becomes active. Otherwise it is routed to O: the output the active scene is
// on, else the first enabled output; with no O it just becomes active.
SceneTake planSceneTake(const SceneList& scenes, const UUID& activeSceneId, int number,
                        const std::vector<SceneTakeOutput>& outputs);

} // namespace quadmapper
